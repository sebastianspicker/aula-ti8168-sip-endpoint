#include "session_private.h"

ls200_status ls200_media_session_select_dtmf_internal(
    const ls200_sdp_negotiated_media *media,
    const ls200_sdp_receive_media *receive_media,
    uint8_t *out_payload_type, uint8_t *out_receive_payload_type,
    int *out_negotiated) {
  size_t index;
  if (media == NULL || receive_media == NULL || out_payload_type == NULL ||
      out_receive_payload_type == NULL || out_negotiated == NULL ||
      media->codec_count != receive_media->codec_count)
    return LS200_STATUS_INVALID_ARGUMENT;
  *out_negotiated = 0;
  for (index = 0U; index < media->codec_count; ++index) {
    if (media->codecs[index].kind == LS200_SDP_CODEC_TELEPHONE_EVENT &&
        media->codecs[index].clock_rate == LS200_G711_SAMPLE_RATE &&
        media->codecs[index].channels == 1U &&
        receive_media->codecs[index].kind == LS200_SDP_CODEC_TELEPHONE_EVENT &&
        receive_media->codecs[index].clock_rate == LS200_G711_SAMPLE_RATE &&
        receive_media->codecs[index].channels == 1U) {
      *out_payload_type = media->codecs[index].payload_type;
      *out_receive_payload_type = receive_media->codecs[index].payload_type;
      *out_negotiated = 1;
      return LS200_STATUS_OK;
    }
  }
  /* Telephone events are optional RTP media. An answer can select the audio
   * codec while declining an incompatible event set; keep audio available and
   * leave the DTMF sender and receiver disabled for that dialog. */
  return LS200_STATUS_OK;
}
