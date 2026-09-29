#include "aula_sipd/rtp.h"
#include "pacing_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct aula_rtp_queue_entry {
  aula_rtp_packet packet;
  uint8_t *payload;
  struct aula_rtp_queue_entry *next;
} aula_rtp_queue_entry;

struct aula_rtp_send_queue {
  aula_rtp_pacing_config config;
  aula_rtp_queue_entry *head;
  aula_rtp_queue_entry *tail;
  uint8_t *last_payload;
  aula_rtp_send_queue_stats stats;
};

struct aula_rtp_send_batch {
  aula_rtp_send_queue *queue;
  aula_rtp_queue_entry *head;
  aula_rtp_queue_entry *tail;
  uint32_t expected_packets;
  uint32_t expected_bytes;
  uint32_t staged_packets;
  uint32_t staged_bytes;
  uint64_t now_ns;
};

static void pacing_secure_zero(void *memory, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)memory;
  while (length > 0U) { *cursor = 0U; ++cursor; --length; }
}

static void aula_rtp_queue_entry_destroy(aula_rtp_queue_entry *entry) {
  if (entry == NULL) return;
  if (entry->payload != NULL) {
    pacing_secure_zero(entry->payload, entry->packet.payload.length);
    free(entry->payload);
  }
  free(entry);
}

static int aula_rtp_queue_config_valid(const aula_rtp_pacing_config *config) {
  return config != NULL && config->packet_interval_ns != 0U &&
         config->maximum_queue_packets != 0U && config->maximum_queue_packets <= 4096U &&
         config->maximum_queue_bytes != 0U &&
         config->maximum_queue_bytes <= (uint32_t)(4096U * AULA_SIPD_MAX_RTP_PACKET_BYTES);
}

static int aula_rtp_queue_packet_is_valid(const aula_rtp_packet *packet) {
  return packet != NULL && packet->header.payload_type <= 127U &&
         packet->header.marker <= 1U &&
         (packet->header.header_bytes == 0U || packet->header.header_bytes == 12U) &&
         (packet->payload.length == 0U || packet->payload.data != NULL) &&
         packet->payload.length <= AULA_SIPD_MAX_RTP_PACKET_BYTES - 12U;
}

static int aula_rtp_queue_is_full(const aula_rtp_send_queue *queue,
                                   size_t payload_length) {
  return queue->stats.queued_packets >= queue->config.maximum_queue_packets ||
         payload_length > queue->config.maximum_queue_bytes - queue->stats.queued_bytes;
}

static aula_rtp_queue_entry *aula_rtp_queue_entry_create(const aula_rtp_packet *packet) {
  aula_rtp_queue_entry *entry;
  entry = (aula_rtp_queue_entry *)calloc(1U, sizeof(*entry));
  if (entry == NULL) return NULL;
  if (packet->payload.length != 0U) {
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

static void pacing_account_drops(aula_rtp_send_queue *queue,
                                 uint32_t packet_count) {
  queue->stats.dropped_packets =
      UINT64_MAX - queue->stats.dropped_packets < packet_count
          ? UINT64_MAX
          : queue->stats.dropped_packets + packet_count;
}

aula_status aula_rtp_send_queue_batch_begin(
    aula_rtp_send_queue *queue, uint32_t packet_count, uint32_t payload_bytes,
    uint64_t now_ns, aula_rtp_send_batch **out_batch) {
  aula_rtp_send_batch *batch;
  if (queue == NULL || packet_count == 0U || payload_bytes == 0U ||
      out_batch == NULL || *out_batch != NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (packet_count > queue->config.maximum_queue_packets -
                         queue->stats.queued_packets ||
      payload_bytes > queue->config.maximum_queue_bytes -
                          queue->stats.queued_bytes) {
    pacing_account_drops(queue, packet_count);
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  batch = (aula_rtp_send_batch *)calloc(1U, sizeof(*batch));
  if (batch == NULL) {
    pacing_account_drops(queue, packet_count);
    return AULA_STATUS_INTERNAL_ERROR;
  }
  batch->queue = queue;
  batch->expected_packets = packet_count;
  batch->expected_bytes = payload_bytes;
  batch->now_ns = now_ns;
  *out_batch = batch;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_batch_stage(aula_rtp_send_batch *batch,
                                        const aula_rtp_packet *packet) {
  aula_rtp_queue_entry *entry;
  if (batch == NULL || !aula_rtp_queue_packet_is_valid(packet) ||
      batch->staged_packets >= batch->expected_packets ||
      packet->payload.length > UINT32_MAX - batch->staged_bytes)
    return AULA_STATUS_INVALID_ARGUMENT;
  entry = aula_rtp_queue_entry_create(packet);
  if (entry == NULL) return AULA_STATUS_INTERNAL_ERROR;
  if (batch->tail == NULL) batch->head = entry;
  else batch->tail->next = entry;
  batch->tail = entry;
  ++batch->staged_packets;
  batch->staged_bytes += (uint32_t)packet->payload.length;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_batch_commit(aula_rtp_send_batch *batch) {
  aula_rtp_send_queue *queue;
  if (batch == NULL || batch->staged_packets != batch->expected_packets ||
      batch->staged_bytes != batch->expected_bytes)
    return AULA_STATUS_STATE_ERROR;
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
  return AULA_STATUS_OK;
}

void aula_rtp_send_batch_abort(aula_rtp_send_batch *batch) {
  aula_rtp_queue_entry *entry;
  if (batch == NULL) return;
  while ((entry = batch->head) != NULL) {
    batch->head = entry->next;
    aula_rtp_queue_entry_destroy(entry);
  }
  pacing_account_drops(batch->queue, batch->expected_packets);
  (void)memset(batch, 0, sizeof(*batch));
  free(batch);
}

aula_status aula_rtp_send_queue_create(const aula_rtp_pacing_config *config,
                                         aula_rtp_send_queue **out_queue) {
  aula_rtp_send_queue *queue;
  if (!aula_rtp_queue_config_valid(config) || out_queue == NULL || *out_queue != NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  queue = (aula_rtp_send_queue *)calloc(1U, sizeof(*queue));
  if (queue == NULL) return AULA_STATUS_INTERNAL_ERROR;
  queue->config = *config;
  *out_queue = queue;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_queue_enqueue(aula_rtp_send_queue *queue,
                                          const aula_rtp_packet *packet, uint64_t now_ns) {
  aula_rtp_queue_entry *entry;
  if (queue == NULL || !aula_rtp_queue_packet_is_valid(packet)) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_rtp_queue_is_full(queue, packet->payload.length)) {
    queue->stats.dropped_packets++;
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  entry = aula_rtp_queue_entry_create(packet);
  if (entry == NULL) return AULA_STATUS_INTERNAL_ERROR;
  if (queue->tail == NULL) queue->head = entry; else queue->tail->next = entry;
  queue->tail = entry;
  queue->stats.queued_packets++;
  queue->stats.queued_bytes += (uint32_t)packet->payload.length;
  if (queue->stats.next_send_ns == 0U) queue->stats.next_send_ns = now_ns;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_queue_dequeue_bounded(aula_rtp_send_queue *queue,
    uint64_t now_ns, uint32_t max_burst, aula_rtp_packet *out_packet) {
  aula_rtp_queue_entry *entry;
  uint64_t credit;
  uint64_t earliest;
  if (queue == NULL || out_packet == NULL || max_burst == 0U || max_burst > 8U)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (queue->head == NULL) return AULA_STATUS_AGAIN;
  if (now_ns < queue->stats.next_send_ns) return AULA_STATUS_AGAIN;
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
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_queue_dequeue(aula_rtp_send_queue *queue,
                                          uint64_t now_ns, aula_rtp_packet *out_packet) {
  return aula_rtp_send_queue_dequeue_bounded(queue, now_ns, 1U, out_packet);
}

aula_status aula_rtp_send_queue_get_stats(const aula_rtp_send_queue *queue,
                                            aula_rtp_send_queue_stats *out_stats) {
  if (queue == NULL || out_stats == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_stats = queue->stats;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_send_queue_discard_all(aula_rtp_send_queue *queue) {
  aula_rtp_queue_entry *entry;
  uint32_t discarded;
  if (queue == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  discarded = queue->stats.queued_packets;
  while ((entry = queue->head) != NULL) {
    queue->head = entry->next;
    aula_rtp_queue_entry_destroy(entry);
  }
  queue->tail = NULL;
  queue->stats.queued_packets = 0U;
  queue->stats.queued_bytes = 0U;
  queue->stats.next_send_ns = 0U;
  queue->stats.dropped_packets =
      UINT64_MAX - queue->stats.dropped_packets < discarded
      ? UINT64_MAX : queue->stats.dropped_packets + discarded;
  return AULA_STATUS_OK;
}

void aula_rtp_send_queue_destroy(aula_rtp_send_queue *queue) {
  aula_rtp_queue_entry *entry;
  if (queue == NULL) return;
  while ((entry = queue->head) != NULL) {
    queue->head = entry->next;
    aula_rtp_queue_entry_destroy(entry);
  }
  free(queue->last_payload);
  free(queue);
}
