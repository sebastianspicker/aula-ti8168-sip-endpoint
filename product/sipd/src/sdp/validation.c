#include "sdp_internal.h"

#include <stdlib.h>
#include <string.h>

int ls200_sdp_media_has_kind(const ls200_sdp_stored_media *media,
                                    ls200_sdp_codec_kind kind) {
  size_t index;
  for (index = 0U; media != NULL && index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == kind) {
      return 1;
    }
  }
  return 0;
}

static ls200_status ls200_sdp_validate_media_transport(const ls200_sdp_stored_media *media) {
  if (media == NULL || !media->present || media->value.rtp_port == 0U ||
      (media->value.rtp_port & 1U) != 0U) return LS200_STATUS_INVALID_DATA;
  if (media->value.rtcp_mux) return media->value.rtcp_port == media->value.rtp_port ?
                                      LS200_STATUS_OK : LS200_STATUS_INVALID_DATA;
  return media->value.rtp_port != 65535U &&
                 media->value.rtcp_port == (uint16_t)(media->value.rtp_port + 1U) ?
             LS200_STATUS_OK : LS200_STATUS_INVALID_DATA;
}

static ls200_status ls200_sdp_validate_media_srtp(const ls200_sdp_stored_media *media) {
  if (media == NULL) return LS200_STATUS_INVALID_DATA;
  if (media->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_AVP) {
    return media->srtp.present ? LS200_STATUS_INVALID_DATA : LS200_STATUS_OK;
  }
  if (media->value.profile != LS200_SDP_MEDIA_PROFILE_RTP_SAVP) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (!ls200_sdp_srtp_is_available()) return LS200_STATUS_UNSUPPORTED;
  return media->srtp.present && media->srtp.crypto_tag != 0U ? LS200_STATUS_OK :
                                                               LS200_STATUS_INVALID_DATA;
}

static ls200_status ls200_sdp_validate_video_offer(const ls200_sdp_stored_media *media) {
  size_t index;
  if (media->value.codec_count != 1U || !ls200_sdp_media_has_kind(media, LS200_SDP_CODEC_H264))
    return LS200_STATUS_UNSUPPORTED;
  for (index = 0U; index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == LS200_SDP_CODEC_H264 &&
        !ls200_sdp_h264_fmtp_is_compatible(media->codecs[index].format_parameters))
      return LS200_STATUS_UNSUPPORTED;
  }
  return LS200_STATUS_OK;
}
ls200_status ls200_sdp_validate_media(const ls200_sdp_stored_media *media,
                                             int video) {
  ls200_status status = ls200_sdp_validate_media_transport(media);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_sdp_validate_media_srtp(media);
  if (status != LS200_STATUS_OK) return status;
  if (video) return ls200_sdp_validate_video_offer(media);
  return ls200_sdp_media_has_kind(media, LS200_SDP_CODEC_PCMU) ||
                 ls200_sdp_media_has_kind(media, LS200_SDP_CODEC_PCMA) ?
             LS200_STATUS_OK : LS200_STATUS_UNSUPPORTED;
}

static ls200_status ls200_sdp_validate_declared_payloads(const ls200_sdp_stored_media *media) {
  size_t index;
  for (index = 0U; index < 128U; ++index) {
    if (media->declared_payload_types[index] != 0U &&
        ls200_sdp_find_codec_const(media, (uint8_t)index) == NULL &&
        media->mapped_payload_types[index] == 0U &&
        (media->is_video || index != 9U)) return LS200_STATUS_UNSUPPORTED;
  }
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_validate_answer_codecs(const ls200_sdp_stored_media *media,
                                                      int video) {
  size_t index;
  int has_compatible_h264 = 0;
  for (index = 0U; index < media->value.codec_count; ++index) {
    const ls200_sdp_codec *codec = &media->codecs[index];
    if ((video && codec->kind != LS200_SDP_CODEC_H264) ||
        (!video && codec->kind == LS200_SDP_CODEC_H264)) return LS200_STATUS_INVALID_DATA;
    if (codec->kind == LS200_SDP_CODEC_H264 &&
        ls200_sdp_h264_fmtp_is_compatible(codec->format_parameters))
      has_compatible_h264 = 1;
  }
  return !video || has_compatible_h264 ? LS200_STATUS_OK :
                                         LS200_STATUS_UNSUPPORTED;
}

static int ls200_sdp_answer_codec_reason(
    const ls200_sdp_stored_media *media, int video) {
  size_t index;
  if (ls200_sdp_validate_declared_payloads(media) != LS200_STATUS_OK) return 4;
  for (index = 0U; index < media->value.codec_count; ++index) {
    if ((video && media->codecs[index].kind != LS200_SDP_CODEC_H264) ||
        (!video && media->codecs[index].kind == LS200_SDP_CODEC_H264)) return 6;
  }
  return video && ls200_sdp_validate_answer_codecs(media, 1) != LS200_STATUS_OK ?
             5 : 0;
}

static int ls200_sdp_offer_codec_reason(
    const ls200_sdp_stored_media *media, int video) {
  if (video && ls200_sdp_validate_answer_codecs(media, 1) != LS200_STATUS_OK)
    return 5;
  if (!video && !ls200_sdp_media_has_kind(media, LS200_SDP_CODEC_PCMU) &&
      !ls200_sdp_media_has_kind(media, LS200_SDP_CODEC_PCMA)) return 7;
  if (video && media->value.codec_count != 1U) return 8;
  return 0;
}

int ls200_sdp_validation_reason(const ls200_sdp_stored_media *media,
                                int strict_offer, int video) {
  if (ls200_sdp_validate_media_transport(media) != LS200_STATUS_OK) return 1;
  if (ls200_sdp_validate_media_srtp(media) != LS200_STATUS_OK) return 2;
  if (media->value.codec_count == 0U) return 3;
  return strict_offer ? ls200_sdp_offer_codec_reason(media, video) :
                        ls200_sdp_answer_codec_reason(media, video);
}

ls200_status ls200_sdp_validate_answer_media(
    const ls200_sdp_stored_media *media, int video) {
  ls200_status status = ls200_sdp_validate_media_transport(media);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_sdp_validate_media_srtp(media);
  if (status != LS200_STATUS_OK) return status;
  if (media->value.codec_count == 0U) return LS200_STATUS_UNSUPPORTED;
  status = ls200_sdp_validate_declared_payloads(media);
  return status == LS200_STATUS_OK ? ls200_sdp_validate_answer_codecs(media, video) : status;
}

ls200_status ls200_sdp_set_direction(ls200_sdp_stored_media *media,
                                            const char *value,
                                            ls200_sdp_direction *session_direction,
                                            int *has_session_direction) {
  ls200_sdp_direction direction;
  if (ls200_sdp_ascii_equal(value, "sendrecv")) {
    direction = LS200_SDP_SENDRECV;
  } else if (ls200_sdp_ascii_equal(value, "sendonly")) {
    direction = LS200_SDP_SENDONLY;
  } else if (ls200_sdp_ascii_equal(value, "recvonly")) {
    direction = LS200_SDP_RECVONLY;
  } else if (ls200_sdp_ascii_equal(value, "inactive")) {
    direction = LS200_SDP_INACTIVE;
  } else {
    return LS200_STATUS_INVALID_DATA;
  }
  if (media == NULL) {
    *session_direction = direction;
    *has_session_direction = 1;
  } else {
    media->value.direction = direction;
    media->direction_explicit = 1;
  }
  return LS200_STATUS_OK;
}
