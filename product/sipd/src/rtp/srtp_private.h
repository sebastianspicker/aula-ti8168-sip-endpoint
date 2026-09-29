#ifndef AULA_SIPD_RTP_SRTP_PRIVATE_H
#define AULA_SIPD_RTP_SRTP_PRIVATE_H

#include "aula_sipd/rtp.h"

#define AULA_RTP_SRTP_MAX_RTP_LIFETIME (UINT64_C(1) << 48U)
#define AULA_RTP_SRTP_MAX_RTCP_LIFETIME (UINT64_C(1) << 31U)

typedef struct aula_rtp_srtp_direction_usage {
  uint64_t rtp_limit;
  uint64_t rtcp_limit;
  uint64_t rtp_packets;
  uint64_t rtcp_packets;
} aula_rtp_srtp_direction_usage;

typedef struct aula_rtp_srtp_limits {
  uint64_t local_outbound;
  uint64_t remote_inbound;
} aula_rtp_srtp_limits;

aula_status aula_rtp_srtp_usage_check(
    const aula_rtp_srtp_direction_usage *usage);
void aula_rtp_srtp_usage_success(aula_rtp_srtp_direction_usage *usage,
                                  int rtcp);
aula_status aula_rtp_session_configure_srtp_with_limits(
    aula_rtp_session *session, const aula_rtp_srtp_config *config,
    const aula_rtp_srtp_limits *limits);

#endif
