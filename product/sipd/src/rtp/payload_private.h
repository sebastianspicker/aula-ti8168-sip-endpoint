#ifndef LS200_SIPD_RTP_PAYLOAD_PRIVATE_H
#define LS200_SIPD_RTP_PAYLOAD_PRIVATE_H

#include "ls200_sipd/rtp.h"

ls200_status ls200_rtp_session_configure_directional_payloads(
    ls200_rtp_session *session, const ls200_rtp_payload_set *inbound,
    const ls200_rtp_payload_set *outbound);

#endif
