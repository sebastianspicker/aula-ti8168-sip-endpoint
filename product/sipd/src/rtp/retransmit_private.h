#ifndef LS200_SIPD_RTP_RETRANSMIT_PRIVATE_H
#define LS200_SIPD_RTP_RETRANSMIT_PRIVATE_H
#include "ls200_sipd/rtp.h"

typedef struct ls200_rtp_retransmit_delta {
  uint32_t packets;
  uint32_t payload_bytes;
  uint32_t wire_bytes;
} ls200_rtp_retransmit_delta;

ls200_status ls200_rtp_retransmit_enable(ls200_rtp_session *session, int enabled);
ls200_status ls200_rtp_retransmit_request(ls200_rtp_session *session,
    uint32_t media_ssrc, uint16_t pid, uint16_t blp);
ls200_status ls200_rtp_retransmit_drain(ls200_rtp_session *session,
    uint64_t now_ns, ls200_rtp_retransmit_delta *out_delta);
void ls200_rtp_retransmit_store(ls200_rtp_session *session,
    const ls200_rtp_packet *packet, ls200_bytes wire, uint64_t now_ns);
void ls200_rtp_retransmit_reset(ls200_rtp_session *session);
void ls200_rtp_retransmit_destroy(ls200_rtp_session *session);
#endif
