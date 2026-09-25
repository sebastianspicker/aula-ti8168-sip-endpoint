#include "rx_playout.h"

#include <string.h>

static int sequence_before(uint16_t left, uint16_t right) {
  return (int16_t)(left - right) < 0;
}

static int find_sequence(const ls200_rx_playout *playout, uint16_t sequence) {
  unsigned int index;
  for (index = 0U; index < LS200_RX_PLAYOUT_MAX_PACKETS; ++index) {
    if (playout->packets[index].payload_length != 0U &&
        playout->packets[index].header.sequence_number == sequence) return (int)index;
  }
  return -1;
}

static int oldest_packet(const ls200_rx_playout *playout) {
  unsigned int index;
  int oldest = -1;
  for (index = 0U; index < LS200_RX_PLAYOUT_MAX_PACKETS; ++index) {
    if (playout->packets[index].payload_length == 0U) continue;
    if (oldest < 0 || playout->packets[index].arrived_ns <
        playout->packets[(unsigned int)oldest].arrived_ns) oldest = (int)index;
  }
  return oldest;
}

static uint64_t playout_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

void ls200_rx_playout_init(ls200_rx_playout *playout, uint64_t packet_interval_ns,
                           uint64_t reorder_hold_ns) {
  if (playout == NULL) return;
  (void)memset(playout, 0, sizeof(*playout));
  playout->packet_interval_ns = packet_interval_ns;
  playout->reorder_hold_ns = reorder_hold_ns;
}

void ls200_rx_playout_reset(ls200_rx_playout *playout) {
  uint64_t interval;
  uint64_t hold;
  if (playout == NULL) return;
  interval = playout->packet_interval_ns;
  hold = playout->reorder_hold_ns;
  (void)memset(playout, 0, sizeof(*playout));
  playout->packet_interval_ns = interval;
  playout->reorder_hold_ns = hold;
}

ls200_status ls200_rx_playout_push(ls200_rx_playout *playout,
                                   const ls200_rtp_packet *packet,
                                   uint64_t now_ns) {
  unsigned int index;
  int slot = -1;
  if (playout == NULL || packet == NULL || packet->payload.data == NULL ||
      packet->payload.length == 0U ||
      packet->payload.length > sizeof(playout->packets[0].payload)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (playout->have_expected_sequence != 0 &&
      sequence_before(packet->header.sequence_number, playout->expected_sequence)) {
    playout->stats.dropped_packets++;
    return LS200_STATUS_AGAIN;
  }
  if (find_sequence(playout, packet->header.sequence_number) >= 0) {
    playout->stats.dropped_packets++;
    return LS200_STATUS_AGAIN;
  }
  for (index = 0U; index < LS200_RX_PLAYOUT_MAX_PACKETS; ++index) {
    if (playout->packets[index].payload_length == 0U) {
      slot = (int)index;
      break;
    }
  }
  if (slot < 0) {
    playout->stats.dropped_packets++;
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  playout->packets[(unsigned int)slot].header = packet->header;
  (void)memcpy(playout->packets[(unsigned int)slot].payload, packet->payload.data,
               packet->payload.length);
  playout->packets[(unsigned int)slot].payload_length = packet->payload.length;
  playout->packets[(unsigned int)slot].arrived_ns = now_ns;
  playout->count++;
  if (playout->have_expected_sequence == 0) {
    playout->expected_sequence = packet->header.sequence_number;
    playout->have_expected_sequence = 1;
    playout->next_playout_ns = now_ns;
  } else if (packet->header.sequence_number != playout->expected_sequence) {
    playout->stats.reordered_packets++;
  }
  return LS200_STATUS_OK;
}

uint64_t ls200_rx_playout_next_action_ns(const ls200_rx_playout *playout,
                                         uint64_t now_ns) {
  int oldest;
  uint64_t hold_until;
  if (playout == NULL || playout->have_expected_sequence == 0 ||
      playout->count == 0U) return UINT64_MAX;
  if (now_ns < playout->next_playout_ns ||
      find_sequence(playout, playout->expected_sequence) >= 0)
    return playout->next_playout_ns;
  oldest = oldest_packet(playout);
  if (oldest < 0) return playout->next_playout_ns;
  hold_until = playout_saturating_add(
      playout->packets[(unsigned int)oldest].arrived_ns,
      playout->reorder_hold_ns);
  return hold_until > now_ns ? hold_until : now_ns;
}

ls200_status ls200_rx_playout_take(ls200_rx_playout *playout, uint64_t now_ns,
                                   ls200_rtp_packet *out_packet,
                                   uint8_t *out_payload,
                                   size_t out_payload_capacity,
                                   int *out_discontinuity) {
  int slot;
  int discontinuity = 0;
  if (playout == NULL || out_packet == NULL || out_payload == NULL ||
      out_discontinuity == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_discontinuity = 0;
  if (playout->have_expected_sequence == 0 || now_ns < playout->next_playout_ns) {
    return LS200_STATUS_AGAIN;
  }
  slot = find_sequence(playout, playout->expected_sequence);
  while (slot < 0 && playout->count != 0U) {
    int oldest = oldest_packet(playout);
    if (oldest < 0 || now_ns < playout->packets[(unsigned int)oldest].arrived_ns ||
        now_ns - playout->packets[(unsigned int)oldest].arrived_ns <
            playout->reorder_hold_ns) return LS200_STATUS_AGAIN;
    playout->expected_sequence = (uint16_t)(playout->expected_sequence + 1U);
    playout->stats.dropped_packets++;
    discontinuity = 1;
    slot = find_sequence(playout, playout->expected_sequence);
  }
  if (slot < 0) {
    playout->stats.underruns++;
    playout->next_playout_ns = playout_saturating_add(
        now_ns, playout->packet_interval_ns);
    return LS200_STATUS_AGAIN;
  }
  if (playout->packets[(unsigned int)slot].payload_length > out_payload_capacity) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  *out_packet = (ls200_rtp_packet){playout->packets[(unsigned int)slot].header,
      {out_payload, playout->packets[(unsigned int)slot].payload_length}};
  (void)memcpy(out_payload, playout->packets[(unsigned int)slot].payload,
               playout->packets[(unsigned int)slot].payload_length);
  playout->packets[(unsigned int)slot].payload_length = 0U;
  playout->count--;
  playout->expected_sequence = (uint16_t)(playout->expected_sequence + 1U);
  playout->next_playout_ns = playout_saturating_add(
      now_ns, playout->packet_interval_ns);
  *out_discontinuity = discontinuity;
  return LS200_STATUS_OK;
}
