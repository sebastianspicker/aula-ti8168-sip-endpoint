#include "retransmit_private.h"
#include "session_internal.h"
#include <stdlib.h>
#include <string.h>

#define RETRANSMIT_SLOTS 256U
#define RETRANSMIT_AGE_NS UINT64_C(500000000)
#define RETRANSMIT_BURST_BYTES 3200U
#define RETRANSMIT_BYTES_PER_SECOND 32000U
#define RETRANSMIT_WIRE_BYTES (AULA_SIPD_MAX_RTP_PACKET_BYTES + AULA_RTP_SRTP_RTP_OVERHEAD_BYTES)

typedef struct retransmit_entry {
  uint8_t wire[RETRANSMIT_WIRE_BYTES];
  uint64_t sent_ns;
  uint64_t target_epoch;
  size_t length;
  uint32_t payload_bytes;
  uint16_t sequence;
  uint8_t payload_type;
  int pending;
  int resent;
} retransmit_entry;

struct aula_rtp_retransmit_cache {
  retransmit_entry entries[RETRANSMIT_SLOTS];
  size_t next;
  size_t cursor;
  uint64_t budget_ns;
  uint32_t budget;
  int budget_initialized;
};

void aula_rtp_retransmit_reset(aula_rtp_session *session) {
  if (session != NULL && session->retransmit != NULL)
    (void)memset(session->retransmit, 0, sizeof(*session->retransmit));
}

void aula_rtp_retransmit_destroy(aula_rtp_session *session) {
  if (session == NULL) return;
  aula_rtp_retransmit_reset(session);
  free(session->retransmit);
  session->retransmit = NULL;
}

aula_status aula_rtp_retransmit_enable(aula_rtp_session *session, int enabled) {
  if (session == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (!enabled) {
    aula_rtp_retransmit_destroy(session);
    return AULA_STATUS_OK;
  }
  if (session->retransmit == NULL)
    session->retransmit = calloc(1U, sizeof(*session->retransmit));
  return session->retransmit != NULL ? AULA_STATUS_OK : AULA_STATUS_INTERNAL_ERROR;
}

void aula_rtp_retransmit_store(aula_rtp_session *session,
    const aula_rtp_packet *packet, aula_bytes wire, uint64_t now_ns) {
  retransmit_entry *entry;
  if (session == NULL || session->retransmit == NULL || packet == NULL ||
      wire.data == NULL || wire.length == 0U || wire.length > RETRANSMIT_WIRE_BYTES ||
      packet->header.ssrc != session->identity.ssrc || !session->remote_target_set)
    return;
  entry = &session->retransmit->entries[session->retransmit->next];
  (void)memset(entry, 0, sizeof(*entry));
  (void)memcpy(entry->wire, wire.data, wire.length);
  entry->length = wire.length;
  entry->payload_bytes = (uint32_t)packet->payload.length;
  entry->sequence = packet->header.sequence_number;
  entry->payload_type = packet->header.payload_type;
  entry->sent_ns = now_ns;
  entry->target_epoch = session->remote_target_epoch;
  session->retransmit->next = (session->retransmit->next + 1U) % RETRANSMIT_SLOTS;
}

aula_status aula_rtp_retransmit_request(aula_rtp_session *session,
    uint32_t media_ssrc, uint16_t pid, uint16_t blp) {
  size_t i;
  if (session == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (session->retransmit == NULL || media_ssrc != session->identity.ssrc)
    return AULA_STATUS_PERMISSION_DENIED;
  for (i = 0U; i < RETRANSMIT_SLOTS; ++i) {
    retransmit_entry *entry = &session->retransmit->entries[i];
    uint16_t distance = (uint16_t)(entry->sequence - pid);
    if (entry->length != 0U && !entry->resent &&
        (distance == 0U || (distance <= 16U &&
          (blp & (uint16_t)(1U << (distance - 1U))) != 0U)))
      entry->pending = 1;
  }
  return AULA_STATUS_OK;
}

static void replenish_budget(struct aula_rtp_retransmit_cache *cache, uint64_t now_ns) {
  uint64_t elapsed;
  uint64_t refill;
  if (!cache->budget_initialized) {
    cache->budget = RETRANSMIT_BURST_BYTES;
    cache->budget_ns = now_ns;
    cache->budget_initialized = 1;
    return;
  }
  if (now_ns <= cache->budget_ns) return;
  elapsed = now_ns - cache->budget_ns;
  refill = elapsed >= UINT64_C(1000000000) ? RETRANSMIT_BURST_BYTES :
      elapsed * RETRANSMIT_BYTES_PER_SECOND / UINT64_C(1000000000);
  if (refill == 0U) return;
  cache->budget = refill >= RETRANSMIT_BURST_BYTES - cache->budget ?
      RETRANSMIT_BURST_BYTES : cache->budget + (uint32_t)refill;
  cache->budget_ns = now_ns;
}

static int entry_is_current(const aula_rtp_session *session,
    const retransmit_entry *entry, uint64_t now_ns) {
  return entry->length != 0U && now_ns >= entry->sent_ns &&
      now_ns - entry->sent_ns <= RETRANSMIT_AGE_NS &&
      entry->target_epoch == session->remote_target_epoch &&
      aula_rtp_payload_is_sendable(session, entry->payload_type);
}

aula_status aula_rtp_retransmit_drain(aula_rtp_session *session,
    uint64_t now_ns, aula_rtp_retransmit_delta *out_delta) {
  size_t i;
  struct aula_rtp_retransmit_cache *cache;
  if (session == NULL || out_delta == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_delta, 0, sizeof(*out_delta));
  cache = session->retransmit;
  if (cache == NULL) return AULA_STATUS_OK;
  if (!session->remote_target_set || session->rtp_socket < 0)
    return AULA_STATUS_PERMISSION_DENIED;
  if (session->srtp_enabled != 0 &&
      aula_rtp_srtp_usage_check(&session->srtp_outbound_usage) != AULA_STATUS_OK)
    return AULA_STATUS_LIMIT_EXCEEDED;
  replenish_budget(cache, now_ns);
  for (i = 0U; i < RETRANSMIT_SLOTS && out_delta->packets < 2U; ++i) {
    retransmit_entry *entry = &cache->entries[cache->cursor];
    aula_status status;
    cache->cursor = (cache->cursor + 1U) % RETRANSMIT_SLOTS;
    if (!entry_is_current(session, entry, now_ns)) {
      (void)memset(entry, 0, sizeof(*entry));
      continue;
    }
    if (!entry->pending || entry->resent || entry->length > cache->budget) continue;
    status = aula_rtp_send_to_address(session->rtp_socket,
        session->remote_target.rtp_address, session->remote_target.rtp_address_length,
        session->remote_target.rtp_port, (aula_bytes){entry->wire, entry->length});
    if (status != AULA_STATUS_OK) return status;
    entry->pending = 0;
    entry->resent = 1;
    cache->budget -= (uint32_t)entry->length;
    ++out_delta->packets;
    out_delta->payload_bytes += entry->payload_bytes;
    out_delta->wire_bytes += (uint32_t)entry->length;
  }
  return AULA_STATUS_OK;
}
