#ifndef AULA_SIPD_SDP_NEGOTIATED_PRIVATE_H
#define AULA_SIPD_SDP_NEGOTIATED_PRIVATE_H

#include "aula_sipd/sdp.h"

#define AULA_SDP_FEEDBACK_NACK UINT32_C(1)
#define AULA_SDP_FEEDBACK_PLI UINT32_C(2)
#define AULA_SDP_FEEDBACK_FIR UINT32_C(4)
#define AULA_SDP_FEEDBACK_TMMBR UINT32_C(8)

aula_status aula_sdp_offer_enable_video_feedback(aula_sdp_session *offer,
                                                   uint32_t mask);
uint32_t aula_sdp_negotiated_video_feedback(
    const aula_sdp_negotiated_session *session);

typedef struct aula_sdp_receive_media {
  aula_sdp_negotiated_codec codecs[AULA_SDP_MAX_SELECTED_CODECS];
  size_t codec_count;
  uint32_t feedback_mask;
} aula_sdp_receive_media;

const aula_sdp_receive_media *aula_sdp_negotiated_get_receive_media(
    const aula_sdp_negotiated_session *session, int video);

aula_status aula_sdp_negotiate_remote_answer_direct_crc(
    const aula_sdp_session *local_offer, aula_bytes remote_answer,
    aula_sdp_negotiated_session **out_negotiated);

#endif
