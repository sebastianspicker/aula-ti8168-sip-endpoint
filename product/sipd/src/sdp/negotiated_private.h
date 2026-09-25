#ifndef LS200_SIPD_SDP_NEGOTIATED_PRIVATE_H
#define LS200_SIPD_SDP_NEGOTIATED_PRIVATE_H

#include "ls200_sipd/sdp.h"

#define LS200_SDP_FEEDBACK_NACK UINT32_C(1)
#define LS200_SDP_FEEDBACK_PLI UINT32_C(2)
#define LS200_SDP_FEEDBACK_FIR UINT32_C(4)
#define LS200_SDP_FEEDBACK_TMMBR UINT32_C(8)

ls200_status ls200_sdp_offer_enable_video_feedback(ls200_sdp_session *offer,
                                                   uint32_t mask);
uint32_t ls200_sdp_negotiated_video_feedback(
    const ls200_sdp_negotiated_session *session);

typedef struct ls200_sdp_receive_media {
  ls200_sdp_negotiated_codec codecs[LS200_SDP_MAX_SELECTED_CODECS];
  size_t codec_count;
  uint32_t feedback_mask;
} ls200_sdp_receive_media;

const ls200_sdp_receive_media *ls200_sdp_negotiated_get_receive_media(
    const ls200_sdp_negotiated_session *session, int video);

ls200_status ls200_sdp_negotiate_remote_answer_direct_crc(
    const ls200_sdp_session *local_offer, ls200_bytes remote_answer,
    ls200_sdp_negotiated_session **out_negotiated);

#endif
