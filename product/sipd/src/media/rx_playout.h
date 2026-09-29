#ifndef AULA_MEDIA_RX_PLAYOUT_H
#define AULA_MEDIA_RX_PLAYOUT_H

#include "aula_sipd/rtp.h"

#define AULA_RX_PLAYOUT_MAX_PACKETS 8U

typedef struct aula_rx_playout_packet {
  aula_rtp_header header;
  uint8_t payload[AULA_SIPD_MAX_RTP_PACKET_BYTES - 12U];
  size_t payload_length;
  uint64_t arrived_ns;
} aula_rx_playout_packet;

typedef struct aula_rx_playout_stats {
  uint64_t reordered_packets;
  uint64_t dropped_packets;
  uint64_t underruns;
} aula_rx_playout_stats;

/* Fixed-size, poll-driven RTP reorder buffer.  It never allocates after the
 * session is created and makes loss explicit once the bounded hold expires. */
typedef struct aula_rx_playout {
  aula_rx_playout_packet packets[AULA_RX_PLAYOUT_MAX_PACKETS];
  uint16_t expected_sequence;
  uint64_t next_playout_ns;
  uint64_t packet_interval_ns;
  uint64_t reorder_hold_ns;
  unsigned int count;
  int have_expected_sequence;
  aula_rx_playout_stats stats;
} aula_rx_playout;

void aula_rx_playout_init(aula_rx_playout *playout, uint64_t packet_interval_ns,
                           uint64_t reorder_hold_ns);
void aula_rx_playout_reset(aula_rx_playout *playout);
aula_status aula_rx_playout_push(aula_rx_playout *playout,
                                   const aula_rtp_packet *packet,
                                   uint64_t now_ns);
/* Returns the first instant at which take can change state.  A missing packet
 * being held for reordering reports the hold expiry instead of an already-due
 * playout instant, preventing a busy poll loop. */
uint64_t aula_rx_playout_next_action_ns(const aula_rx_playout *playout,
                                         uint64_t now_ns);
/* Produces at most one due packet.  discontinuity is set when a missing
 * sequence was declared lost before this packet. */
aula_status aula_rx_playout_take(aula_rx_playout *playout, uint64_t now_ns,
                                   aula_rtp_packet *out_packet,
                                   uint8_t *out_payload,
                                   size_t out_payload_capacity,
                                   int *out_discontinuity);

#endif
