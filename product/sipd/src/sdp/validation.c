#include "sdp_internal.h"

#include <stdlib.h>
#include <string.h>

int aula_sdp_media_has_kind(const aula_sdp_stored_media *media,
                                    aula_sdp_codec_kind kind) {
  size_t index;
  for (index = 0U; media != NULL && index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == kind) {
      return 1;
    }
  }
  return 0;
}

static aula_status aula_sdp_validate_media_transport(const aula_sdp_stored_media *media) {
  if (media == NULL || !media->present || media->value.rtp_port == 0U ||
      (media->value.rtp_port & 1U) != 0U) return AULA_STATUS_INVALID_DATA;
  if (media->value.rtcp_mux) return media->value.rtcp_port == media->value.rtp_port ?
                                      AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
  return media->value.rtp_port != 65535U &&
                 media->value.rtcp_port == (uint16_t)(media->value.rtp_port + 1U) ?
             AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
}

static aula_status aula_sdp_validate_media_srtp(const aula_sdp_stored_media *media) {
  if (media == NULL) return AULA_STATUS_INVALID_DATA;
  if (media->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP) {
    return media->srtp.present ? AULA_STATUS_INVALID_DATA : AULA_STATUS_OK;
  }
  if (media->value.profile != AULA_SDP_MEDIA_PROFILE_RTP_SAVP) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (!aula_sdp_srtp_is_available()) return AULA_STATUS_UNSUPPORTED;
  return media->srtp.present && media->srtp.crypto_tag != 0U ? AULA_STATUS_OK :
                                                               AULA_STATUS_INVALID_DATA;
}

static aula_status aula_sdp_validate_video_offer(const aula_sdp_stored_media *media) {
  size_t index;
  if (media->value.codec_count != 1U || !aula_sdp_media_has_kind(media, AULA_SDP_CODEC_H264))
    return AULA_STATUS_UNSUPPORTED;
  for (index = 0U; index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == AULA_SDP_CODEC_H264 &&
        !aula_sdp_h264_fmtp_is_compatible(media->codecs[index].format_parameters))
      return AULA_STATUS_UNSUPPORTED;
  }
  return AULA_STATUS_OK;
}
aula_status aula_sdp_validate_media(const aula_sdp_stored_media *media,
                                             int video) {
  aula_status status = aula_sdp_validate_media_transport(media);
  if (status != AULA_STATUS_OK) return status;
  status = aula_sdp_validate_media_srtp(media);
  if (status != AULA_STATUS_OK) return status;
  if (video) return aula_sdp_validate_video_offer(media);
  return aula_sdp_media_has_kind(media, AULA_SDP_CODEC_PCMU) ||
                 aula_sdp_media_has_kind(media, AULA_SDP_CODEC_PCMA) ?
             AULA_STATUS_OK : AULA_STATUS_UNSUPPORTED;
}

static aula_status aula_sdp_validate_declared_payloads(const aula_sdp_stored_media *media) {
  size_t index;
  for (index = 0U; index < 128U; ++index) {
    if (media->declared_payload_types[index] != 0U &&
        aula_sdp_find_codec_const(media, (uint8_t)index) == NULL &&
        media->mapped_payload_types[index] == 0U &&
        (media->is_video || index != 9U)) return AULA_STATUS_UNSUPPORTED;
  }
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_validate_answer_codecs(const aula_sdp_stored_media *media,
                                                      int video) {
  size_t index;
  int has_compatible_h264 = 0;
  for (index = 0U; index < media->value.codec_count; ++index) {
    const aula_sdp_codec *codec = &media->codecs[index];
    if ((video && codec->kind != AULA_SDP_CODEC_H264) ||
        (!video && codec->kind == AULA_SDP_CODEC_H264)) return AULA_STATUS_INVALID_DATA;
    if (codec->kind == AULA_SDP_CODEC_H264 &&
        aula_sdp_h264_fmtp_is_compatible(codec->format_parameters))
      has_compatible_h264 = 1;
  }
  return !video || has_compatible_h264 ? AULA_STATUS_OK :
                                         AULA_STATUS_UNSUPPORTED;
}

static int aula_sdp_answer_codec_reason(
    const aula_sdp_stored_media *media, int video) {
  size_t index;
  if (aula_sdp_validate_declared_payloads(media) != AULA_STATUS_OK) return 4;
  for (index = 0U; index < media->value.codec_count; ++index) {
    if ((video && media->codecs[index].kind != AULA_SDP_CODEC_H264) ||
        (!video && media->codecs[index].kind == AULA_SDP_CODEC_H264)) return 6;
  }
  return video && aula_sdp_validate_answer_codecs(media, 1) != AULA_STATUS_OK ?
             5 : 0;
}

static int aula_sdp_offer_codec_reason(
    const aula_sdp_stored_media *media, int video) {
  if (video && aula_sdp_validate_answer_codecs(media, 1) != AULA_STATUS_OK)
    return 5;
  if (!video && !aula_sdp_media_has_kind(media, AULA_SDP_CODEC_PCMU) &&
      !aula_sdp_media_has_kind(media, AULA_SDP_CODEC_PCMA)) return 7;
  if (video && media->value.codec_count != 1U) return 8;
  return 0;
}

int aula_sdp_validation_reason(const aula_sdp_stored_media *media,
                                int strict_offer, int video) {
  if (aula_sdp_validate_media_transport(media) != AULA_STATUS_OK) return 1;
  if (aula_sdp_validate_media_srtp(media) != AULA_STATUS_OK) return 2;
  if (media->value.codec_count == 0U) return 3;
  return strict_offer ? aula_sdp_offer_codec_reason(media, video) :
                        aula_sdp_answer_codec_reason(media, video);
}

aula_status aula_sdp_validate_answer_media(
    const aula_sdp_stored_media *media, int video) {
  aula_status status = aula_sdp_validate_media_transport(media);
  if (status != AULA_STATUS_OK) return status;
  status = aula_sdp_validate_media_srtp(media);
  if (status != AULA_STATUS_OK) return status;
  if (media->value.codec_count == 0U) return AULA_STATUS_UNSUPPORTED;
  status = aula_sdp_validate_declared_payloads(media);
  return status == AULA_STATUS_OK ? aula_sdp_validate_answer_codecs(media, video) : status;
}

aula_status aula_sdp_set_direction(aula_sdp_stored_media *media,
                                            const char *value,
                                            aula_sdp_direction *session_direction,
                                            int *has_session_direction) {
  aula_sdp_direction direction;
  if (aula_sdp_ascii_equal(value, "sendrecv")) {
    direction = AULA_SDP_SENDRECV;
  } else if (aula_sdp_ascii_equal(value, "sendonly")) {
    direction = AULA_SDP_SENDONLY;
  } else if (aula_sdp_ascii_equal(value, "recvonly")) {
    direction = AULA_SDP_RECVONLY;
  } else if (aula_sdp_ascii_equal(value, "inactive")) {
    direction = AULA_SDP_INACTIVE;
  } else {
    return AULA_STATUS_INVALID_DATA;
  }
  if (media == NULL) {
    *session_direction = direction;
    *has_session_direction = 1;
  } else {
    media->value.direction = direction;
    media->direction_explicit = 1;
  }
  return AULA_STATUS_OK;
}
