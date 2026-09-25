#ifndef LS200_SIPD_SDP_SRTP_LIMITS_H
#define LS200_SIPD_SDP_SRTP_LIMITS_H

#include "ls200_sipd/sdp.h"

#define LS200_SDP_SRTP_DEFAULT_LIFETIME (UINT64_C(1) << 48U)

typedef struct ls200_sdp_srtp_lifetimes {
  uint64_t local_outbound;
  uint64_t remote_inbound;
} ls200_sdp_srtp_lifetimes;

int ls200_sdp_parse_srtp_lifetime(const char *text, uint64_t *out_lifetime);
ls200_status ls200_sdp_negotiated_get_srtp_lifetimes(
    const ls200_sdp_negotiated_session *session, int video,
    ls200_sdp_srtp_lifetimes *out_lifetimes);

#endif
