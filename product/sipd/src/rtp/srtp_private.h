#ifndef LS200_SIPD_RTP_SRTP_PRIVATE_H
#define LS200_SIPD_RTP_SRTP_PRIVATE_H

#include "ls200_sipd/rtp.h"

#define LS200_RTP_SRTP_MAX_RTP_LIFETIME (UINT64_C(1) << 48U)
#define LS200_RTP_SRTP_MAX_RTCP_LIFETIME (UINT64_C(1) << 31U)

typedef struct ls200_rtp_srtp_direction_usage {
  uint64_t rtp_limit;
  uint64_t rtcp_limit;
  uint64_t rtp_packets;
  uint64_t rtcp_packets;
} ls200_rtp_srtp_direction_usage;

typedef struct ls200_rtp_srtp_limits {
  uint64_t local_outbound;
  uint64_t remote_inbound;
} ls200_rtp_srtp_limits;

ls200_status ls200_rtp_srtp_usage_check(
    const ls200_rtp_srtp_direction_usage *usage);
void ls200_rtp_srtp_usage_success(ls200_rtp_srtp_direction_usage *usage,
                                  int rtcp);
ls200_status ls200_rtp_session_configure_srtp_with_limits(
    ls200_rtp_session *session, const ls200_rtp_srtp_config *config,
    const ls200_rtp_srtp_limits *limits);

#endif
