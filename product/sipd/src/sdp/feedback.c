#include "sdp_internal.h"

#include <stdio.h>
#include <string.h>

static uint8_t feedback_kind(const char *value) {
  if (strcmp(value, "nack") == 0) return LS200_SDP_FEEDBACK_NACK;
  if (strcmp(value, "nack pli") == 0) return LS200_SDP_FEEDBACK_PLI;
  if (strcmp(value, "ccm fir") == 0) return LS200_SDP_FEEDBACK_FIR;
  if (strcmp(value, "ccm tmmbr") == 0) return LS200_SDP_FEEDBACK_TMMBR;
  return 0U;
}

ls200_status ls200_sdp_parse_feedback(ls200_sdp_stored_media *media, char *value) {
  char *space;
  uint8_t payload = 0U;
  uint8_t mask;
  int wildcard;
  if (media == NULL || value == NULL) return LS200_STATUS_INVALID_DATA;
  space = strchr(value, ' ');
  if (space == NULL || space == value || space[1] == '\0') return LS200_STATUS_INVALID_DATA;
  *space++ = '\0';
  wildcard = strcmp(value, "*") == 0;
  if (!wildcard && !ls200_sdp_parse_payload(value, &payload)) return LS200_STATUS_INVALID_DATA;
  /* Unknown extensions are ignored in their entirety, never by prefix. */
  mask = feedback_kind(space);
  if (wildcard) media->feedback_wildcard |= mask;
  else media->feedback_payload[payload] |= mask;
  return LS200_STATUS_OK;
}

uint32_t ls200_sdp_media_feedback(const ls200_sdp_stored_media *media,
                                  uint8_t payload) {
  if (media == NULL || payload > 127U) return 0U;
  return media->feedback_wildcard | media->feedback_payload[payload];
}

ls200_status ls200_sdp_offer_enable_video_feedback(ls200_sdp_session *offer,
                                                   uint32_t mask) {
  const uint32_t supported = LS200_SDP_FEEDBACK_NACK | LS200_SDP_FEEDBACK_PLI |
                             LS200_SDP_FEEDBACK_FIR;
  size_t index;
  if (offer == NULL || !offer->video.present || (mask & ~supported) != 0U)
    return LS200_STATUS_INVALID_ARGUMENT;
  offer->video.feedback_wildcard = 0U;
  (void)memset(offer->video.feedback_payload, 0, sizeof(offer->video.feedback_payload));
  for (index = 0U; index < offer->video.value.codec_count; ++index) {
    const ls200_sdp_codec *codec = &offer->video.codecs[index];
    if (codec->kind == LS200_SDP_CODEC_H264)
      offer->video.feedback_payload[codec->payload_type] = (uint8_t)mask;
  }
  return LS200_STATUS_OK;
}

uint32_t ls200_sdp_negotiated_video_feedback(
    const ls200_sdp_negotiated_session *session) {
  return session == NULL ? 0U : session->receive_video.feedback_mask;
}

static ls200_status append_feedback_line(ls200_mutable_bytes *output,
                                           const char *payload, const char *kind) {
  char line[64];
  int length = snprintf(line, sizeof(line), "a=rtcp-fb:%s %s\r\n", payload, kind);
  if (length < 0 || (size_t)length >= sizeof(line)) return LS200_STATUS_INTERNAL_ERROR;
  if (output->length > output->capacity ||
      (size_t)length > output->capacity - output->length) return LS200_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(output->data + output->length, line, (size_t)length);
  output->length += (size_t)length;
  return LS200_STATUS_OK;
}

static ls200_status serialize_mask(uint32_t mask, const char *payload,
                                    ls200_mutable_bytes *output) {
  static const char *const names[] = {"nack", "nack pli", "ccm fir", "ccm tmmbr"};
  unsigned index;
  for (index = 0U; index < 4U; ++index) {
    if ((mask & (UINT32_C(1) << index)) != 0U) {
      ls200_status status = append_feedback_line(output, payload, names[index]);
      if (status != LS200_STATUS_OK) return status;
    }
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_serialize_feedback(const ls200_sdp_stored_media *media,
                                          ls200_mutable_bytes *output) {
  size_t index;
  ls200_status status = serialize_mask(media->feedback_wildcard, "*", output);
  for (index = 0U; status == LS200_STATUS_OK && index < media->value.codec_count; ++index) {
    uint8_t payload = media->codecs[index].payload_type;
    char text[4];
    (void)snprintf(text, sizeof(text), "%u", (unsigned)payload);
    status = serialize_mask(media->feedback_payload[payload], text, output);
  }
  return status;
}
