#include "ls200_sipd/rtp.h"
#include "pacing_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct ls200_rtp_queue_entry {
  ls200_rtp_packet packet;
  uint8_t *payload;
  struct ls200_rtp_queue_entry *next;
} ls200_rtp_queue_entry;

struct ls200_rtp_send_queue {
  ls200_rtp_pacing_config config;
  ls200_rtp_queue_entry *head;
  ls200_rtp_queue_entry *tail;
  uint8_t *last_payload;
  ls200_rtp_send_queue_stats stats;
};

struct ls200_rtp_send_batch {
  ls200_rtp_send_queue *queue;
  ls200_rtp_queue_entry *head;
  ls200_rtp_queue_entry *tail;
  uint32_t expected_packets;
  uint32_t expected_bytes;
  uint32_t staged_packets;
  uint32_t staged_bytes;
  uint64_t now_ns;
};

#if defined(LS200_SIPD_TEST_FAULTS) && LS200_SIPD_TEST_FAULTS
static int pacing_fail_allocation_after = -1;

void ls200_rtp_send_queue_test_fail_allocation_after(int allocation_count) {
  pacing_fail_allocation_after = allocation_count;
}

static int pacing_allocation_fails(void) {
  if (pacing_fail_allocation_after < 0) return 0;
  if (pacing_fail_allocation_after == 0) return 1;
  --pacing_fail_allocation_after;
  return 0;
}
#else
static int pacing_allocation_fails(void) { return 0; }
#endif

static void pacing_secure_zero(void *memory, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)memory;
  while (length > 0U) { *cursor = 0U; ++cursor; --length; }
}

static void ls200_rtp_queue_entry_destroy(ls200_rtp_queue_entry *entry) {
  if (entry == NULL) return;
  if (entry->payload != NULL) {
    pacing_secure_zero(entry->payload, entry->packet.payload.length);
    free(entry->payload);
  }
  free(entry);
}

static int ls200_rtp_queue_config_valid(const ls200_rtp_pacing_config *config) {
  return config != NULL && config->packet_interval_ns != 0U &&
         config->maximum_queue_packets != 0U && config->maximum_queue_packets <= 4096U &&
         config->maximum_queue_bytes != 0U &&
         config->maximum_queue_bytes <= (uint32_t)(4096U * LS200_SIPD_MAX_RTP_PACKET_BYTES);
}

static int ls200_rtp_queue_packet_is_valid(const ls200_rtp_packet *packet) {
  return packet != NULL && packet->header.payload_type <= 127U &&
         packet->header.marker <= 1U &&
         (packet->header.header_bytes == 0U || packet->header.header_bytes == 12U) &&
         (packet->payload.length == 0U || packet->payload.data != NULL) &&
         packet->payload.length <= LS200_SIPD_MAX_RTP_PACKET_BYTES - 12U;
}

static int ls200_rtp_queue_is_full(const ls200_rtp_send_queue *queue,
                                   size_t payload_length) {
  return queue->stats.queued_packets >= queue->config.maximum_queue_packets ||
         payload_length > queue->config.maximum_queue_bytes - queue->stats.queued_bytes;
}

static ls200_rtp_queue_entry *ls200_rtp_queue_entry_create(const ls200_rtp_packet *packet) {
  ls200_rtp_queue_entry *entry;
  if (pacing_allocation_fails()) return NULL;
  entry = (ls200_rtp_queue_entry *)calloc(1U, sizeof(*entry));
  if (entry == NULL) return NULL;
  if (packet->payload.length != 0U) {
    if (pacing_allocation_fails()) {
      free(entry);
      return NULL;
    }
    entry->payload = (uint8_t *)malloc(packet->payload.length);
    if (entry->payload == NULL) {
      free(entry);
      return NULL;
    }
    (void)memcpy(entry->payload, packet->payload.data, packet->payload.length);
  }
  entry->packet = *packet;
  entry->packet.payload.data = entry->payload;
  return entry;
}

static void pacing_account_drops(ls200_rtp_send_queue *queue,
                                 uint32_t packet_count) {
  queue->stats.dropped_packets =
      UINT64_MAX - queue->stats.dropped_packets < packet_count
          ? UINT64_MAX
          : queue->stats.dropped_packets + packet_count;
}

ls200_status ls200_rtp_send_queue_batch_begin(
    ls200_rtp_send_queue *queue, uint32_t packet_count, uint32_t payload_bytes,
    uint64_t now_ns, ls200_rtp_send_batch **out_batch) {
  ls200_rtp_send_batch *batch;
  if (queue == NULL || packet_count == 0U || payload_bytes == 0U ||
      out_batch == NULL || *out_batch != NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (packet_count > queue->config.maximum_queue_packets -
                         queue->stats.queued_packets ||
      payload_bytes > queue->config.maximum_queue_bytes -
                          queue->stats.queued_bytes) {
    pacing_account_drops(queue, packet_count);
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  if (pacing_allocation_fails()) {
    pacing_account_drops(queue, packet_count);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  batch = (ls200_rtp_send_batch *)calloc(1U, sizeof(*batch));
  if (batch == NULL) {
    pacing_account_drops(queue, packet_count);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  batch->queue = queue;
  batch->expected_packets = packet_count;
  batch->expected_bytes = payload_bytes;
  batch->now_ns = now_ns;
  *out_batch = batch;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_batch_stage(ls200_rtp_send_batch *batch,
                                        const ls200_rtp_packet *packet) {
  ls200_rtp_queue_entry *entry;
  if (batch == NULL || !ls200_rtp_queue_packet_is_valid(packet) ||
      batch->staged_packets >= batch->expected_packets ||
      packet->payload.length > UINT32_MAX - batch->staged_bytes)
    return LS200_STATUS_INVALID_ARGUMENT;
  entry = ls200_rtp_queue_entry_create(packet);
  if (entry == NULL) return LS200_STATUS_INTERNAL_ERROR;
  if (batch->tail == NULL) batch->head = entry;
  else batch->tail->next = entry;
  batch->tail = entry;
  ++batch->staged_packets;
  batch->staged_bytes += (uint32_t)packet->payload.length;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_batch_commit(ls200_rtp_send_batch *batch) {
  ls200_rtp_send_queue *queue;
  if (batch == NULL || batch->staged_packets != batch->expected_packets ||
      batch->staged_bytes != batch->expected_bytes)
    return LS200_STATUS_STATE_ERROR;
  queue = batch->queue;
  if (queue->tail == NULL) queue->head = batch->head;
  else queue->tail->next = batch->head;
  queue->tail = batch->tail;
  queue->stats.queued_packets += batch->staged_packets;
  queue->stats.queued_bytes += batch->staged_bytes;
  if (queue->stats.next_send_ns == 0U) queue->stats.next_send_ns = batch->now_ns;
  batch->head = NULL;
  batch->tail = NULL;
  free(batch);
  return LS200_STATUS_OK;
}

void ls200_rtp_send_batch_abort(ls200_rtp_send_batch *batch) {
  ls200_rtp_queue_entry *entry;
  if (batch == NULL) return;
  while ((entry = batch->head) != NULL) {
    batch->head = entry->next;
    ls200_rtp_queue_entry_destroy(entry);
  }
  pacing_account_drops(batch->queue, batch->expected_packets);
  (void)memset(batch, 0, sizeof(*batch));
  free(batch);
}

ls200_status ls200_rtp_send_queue_create(const ls200_rtp_pacing_config *config,
                                         ls200_rtp_send_queue **out_queue) {
  ls200_rtp_send_queue *queue;
  if (!ls200_rtp_queue_config_valid(config) || out_queue == NULL || *out_queue != NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  queue = (ls200_rtp_send_queue *)calloc(1U, sizeof(*queue));
  if (queue == NULL) return LS200_STATUS_INTERNAL_ERROR;
  queue->config = *config;
  *out_queue = queue;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_queue_enqueue(ls200_rtp_send_queue *queue,
                                          const ls200_rtp_packet *packet, uint64_t now_ns) {
  ls200_rtp_queue_entry *entry;
  if (queue == NULL || !ls200_rtp_queue_packet_is_valid(packet)) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_rtp_queue_is_full(queue, packet->payload.length)) {
    queue->stats.dropped_packets++;
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  entry = ls200_rtp_queue_entry_create(packet);
  if (entry == NULL) return LS200_STATUS_INTERNAL_ERROR;
  if (queue->tail == NULL) queue->head = entry; else queue->tail->next = entry;
  queue->tail = entry;
  queue->stats.queued_packets++;
  queue->stats.queued_bytes += (uint32_t)packet->payload.length;
  if (queue->stats.next_send_ns == 0U) queue->stats.next_send_ns = now_ns;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_queue_dequeue_bounded(ls200_rtp_send_queue *queue,
    uint64_t now_ns, uint32_t max_burst, ls200_rtp_packet *out_packet) {
  ls200_rtp_queue_entry *entry;
  uint64_t credit;
  uint64_t earliest;
  if (queue == NULL || out_packet == NULL || max_burst == 0U || max_burst > 8U)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (queue->head == NULL) return LS200_STATUS_AGAIN;
  if (now_ns < queue->stats.next_send_ns) return LS200_STATUS_AGAIN;
  entry = queue->head;
  queue->head = entry->next;
  if (queue->head == NULL) queue->tail = NULL;
  free(queue->last_payload);
  queue->last_payload = entry->payload;
  *out_packet = entry->packet;
  out_packet->payload.data = queue->last_payload;
  queue->stats.queued_packets--;
  queue->stats.queued_bytes -= (uint32_t)entry->packet.payload.length;
  queue->stats.sent_packets++;
  credit = queue->config.packet_interval_ns > UINT64_MAX / max_burst ?
      UINT64_MAX : queue->config.packet_interval_ns * (max_burst - 1U);
  earliest = now_ns > credit ? now_ns - credit : 0U;
  if (earliest > queue->stats.next_send_ns) queue->stats.next_send_ns = earliest;
  if (UINT64_MAX - queue->stats.next_send_ns < queue->config.packet_interval_ns) {
    queue->stats.next_send_ns = UINT64_MAX;
  } else {
    queue->stats.next_send_ns += queue->config.packet_interval_ns;
  }
  free(entry);
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_queue_dequeue(ls200_rtp_send_queue *queue,
                                          uint64_t now_ns, ls200_rtp_packet *out_packet) {
  return ls200_rtp_send_queue_dequeue_bounded(queue, now_ns, 1U, out_packet);
}

ls200_status ls200_rtp_send_queue_get_stats(const ls200_rtp_send_queue *queue,
                                            ls200_rtp_send_queue_stats *out_stats) {
  if (queue == NULL || out_stats == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_stats = queue->stats;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtp_send_queue_discard_all(ls200_rtp_send_queue *queue) {
  ls200_rtp_queue_entry *entry;
  uint32_t discarded;
  if (queue == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  discarded = queue->stats.queued_packets;
  while ((entry = queue->head) != NULL) {
    queue->head = entry->next;
    ls200_rtp_queue_entry_destroy(entry);
  }
  queue->tail = NULL;
  queue->stats.queued_packets = 0U;
  queue->stats.queued_bytes = 0U;
  queue->stats.next_send_ns = 0U;
  queue->stats.dropped_packets =
      UINT64_MAX - queue->stats.dropped_packets < discarded
      ? UINT64_MAX : queue->stats.dropped_packets + discarded;
  return LS200_STATUS_OK;
}

void ls200_rtp_send_queue_destroy(ls200_rtp_send_queue *queue) {
  ls200_rtp_queue_entry *entry;
  if (queue == NULL) return;
  while ((entry = queue->head) != NULL) {
    queue->head = entry->next;
    ls200_rtp_queue_entry_destroy(entry);
  }
  free(queue->last_payload);
  free(queue);
}
