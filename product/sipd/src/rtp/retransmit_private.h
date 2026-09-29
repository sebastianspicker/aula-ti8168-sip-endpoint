#ifndef AULA_SIPD_RTP_RETRANSMIT_PRIVATE_H
#define AULA_SIPD_RTP_RETRANSMIT_PRIVATE_H
#include "aula_sipd/rtp.h"

typedef struct aula_rtp_retransmit_delta {
  uint32_t packets;
  uint32_t payload_bytes;
  uint32_t wire_bytes;
} aula_rtp_retransmit_delta;

aula_status aula_rtp_retransmit_enable(aula_rtp_session *session, int enabled);
aula_status aula_rtp_retransmit_request(aula_rtp_session *session,
    uint32_t media_ssrc, uint16_t pid, uint16_t blp);
aula_status aula_rtp_retransmit_drain(aula_rtp_session *session,
    uint64_t now_ns, aula_rtp_retransmit_delta *out_delta);
void aula_rtp_retransmit_store(aula_rtp_session *session,
    const aula_rtp_packet *packet, aula_bytes wire, uint64_t now_ns);
void aula_rtp_retransmit_reset(aula_rtp_session *session);
void aula_rtp_retransmit_destroy(aula_rtp_session *session);
#endif
