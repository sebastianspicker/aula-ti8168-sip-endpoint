#ifndef AULA_SIPD_SDP_SRTP_LIMITS_H
#define AULA_SIPD_SDP_SRTP_LIMITS_H

#include "aula_sipd/sdp.h"

#define AULA_SDP_SRTP_DEFAULT_LIFETIME (UINT64_C(1) << 48U)

typedef struct aula_sdp_srtp_lifetimes {
  uint64_t local_outbound;
  uint64_t remote_inbound;
} aula_sdp_srtp_lifetimes;

int aula_sdp_parse_srtp_lifetime(const char *text, uint64_t *out_lifetime);
aula_status aula_sdp_negotiated_get_srtp_lifetimes(
    const aula_sdp_negotiated_session *session, int video,
    aula_sdp_srtp_lifetimes *out_lifetimes);

#endif
