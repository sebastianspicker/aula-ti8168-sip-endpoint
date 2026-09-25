#include "session_private.h"
#include "h264_profile_private.h"

#include <string.h>

static ls200_status select_video(
    const ls200_sdp_negotiated_media *media,
    const ls200_sdp_receive_media *receive_media,
    uint8_t *out_payload_type, uint8_t *out_receive_payload_type) {
  size_t index;
  if (media == NULL || receive_media == NULL || out_payload_type == NULL ||
      out_receive_payload_type == NULL ||
      media->codec_count != receive_media->codec_count)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (!media->h264.present || media->h264.packetization_mode != 1)
    return LS200_STATUS_UNSUPPORTED;
  for (index = 0U; index < media->codec_count; ++index) {
    if (media->codecs[index].kind == LS200_SDP_CODEC_H264 &&
        media->codecs[index].clock_rate == 90000U &&
        receive_media->codecs[index].kind == LS200_SDP_CODEC_H264 &&
        receive_media->codecs[index].clock_rate == 90000U) {
      *out_payload_type = media->codecs[index].payload_type;
      *out_receive_payload_type = receive_media->codecs[index].payload_type;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_UNSUPPORTED;
}

static ls200_status select_audio(
    const ls200_sdp_negotiated_media *media,
    const ls200_sdp_receive_media *receive_media,
    uint8_t *out_payload_type, uint8_t *out_receive_payload_type,
    int *out_use_pcma) {
  size_t index;
  if (media == NULL || receive_media == NULL || out_payload_type == NULL ||
      out_receive_payload_type == NULL || out_use_pcma == NULL ||
      media->codec_count != receive_media->codec_count)
    return LS200_STATUS_INVALID_ARGUMENT;
  for (index = 0U; index < media->codec_count; ++index) {
    const ls200_sdp_negotiated_codec *codec = &media->codecs[index];
    const ls200_sdp_negotiated_codec *receive = &receive_media->codecs[index];
    if (codec->clock_rate != LS200_G711_SAMPLE_RATE || codec->channels != 1U ||
        receive->kind != codec->kind || receive->clock_rate != codec->clock_rate ||
        receive->channels != codec->channels)
      continue;
    if (codec->kind == LS200_SDP_CODEC_PCMU ||
        codec->kind == LS200_SDP_CODEC_PCMA) {
      *out_payload_type = codec->payload_type;
      *out_receive_payload_type = receive->payload_type;
      *out_use_pcma = codec->kind == LS200_SDP_CODEC_PCMA;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_UNSUPPORTED;
}

static int profile_level_id_is_valid(const char profile_level_id[7]) {
  return ls200_h264_profile_level_id_valid(profile_level_id);
}

ls200_status ls200_media_session_prepare_select_codecs_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_receive_media *receive_video,
    const ls200_sdp_negotiated_media *audio,
    const ls200_sdp_receive_media *receive_audio) {
  ls200_status status = select_video(
      video, receive_video, &session->video_payload_type,
      &session->video_receive_payload_type);
  if (status == LS200_STATUS_OK &&
      profile_level_id_is_valid(video->h264.profile_level_id)) {
    (void)memcpy(session->negotiated_h264_profile_level_id,
                 video->h264.profile_level_id,
                 sizeof(session->negotiated_h264_profile_level_id));
  } else if (status == LS200_STATUS_OK) {
    status = LS200_STATUS_INVALID_DATA;
  }
  if (status == LS200_STATUS_OK)
    status = select_audio(audio, receive_audio, &session->audio_payload_type,
                          &session->audio_receive_payload_type,
                          &session->use_pcma);
  if (status == LS200_STATUS_OK)
    status = ls200_media_session_select_dtmf_internal(
        audio, receive_audio, &session->dtmf_payload_type,
        &session->dtmf_receive_payload_type, &session->dtmf_negotiated);
  if (status == LS200_STATUS_OK) session->directional_payloads_configured = 1;
  return status;
}
