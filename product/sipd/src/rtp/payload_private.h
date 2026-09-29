#ifndef AULA_SIPD_RTP_PAYLOAD_PRIVATE_H
#define AULA_SIPD_RTP_PAYLOAD_PRIVATE_H

#include "aula_sipd/rtp.h"

aula_status aula_rtp_session_configure_directional_payloads(
    aula_rtp_session *session, const aula_rtp_payload_set *inbound,
    const aula_rtp_payload_set *outbound);

#endif
