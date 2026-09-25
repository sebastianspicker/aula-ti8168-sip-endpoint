#include "session_private.h"

typedef void (*payload_reader)(const void *codecs, size_t index,
                               uint8_t *payload_type, uint32_t *clock_rate);

static void read_local(const void *codecs, size_t index,
                       uint8_t *payload_type, uint32_t *clock_rate) {
  const ls200_sdp_codec *values = (const ls200_sdp_codec *)codecs;
  *payload_type = values[index].payload_type;
  *clock_rate = values[index].clock_rate;
}

static void read_negotiated(const void *codecs, size_t index,
                            uint8_t *payload_type, uint32_t *clock_rate) {
  const ls200_sdp_negotiated_codec *values =
      (const ls200_sdp_negotiated_codec *)codecs;
  *payload_type = values[index].payload_type;
  *clock_rate = values[index].clock_rate;
}

static ls200_status collect_values(const void *codecs, size_t codec_count,
                                   ls200_media_session_stream *stream,
                                   payload_reader read_payload) {
  size_t index;
  if (codecs == NULL || stream == NULL || read_payload == NULL ||
      codec_count == 0U || codec_count > LS200_SDP_MAX_SELECTED_CODECS)
    return LS200_STATUS_INVALID_DATA;
  for (index = 0U; index < codec_count; ++index) {
    size_t prior;
    uint8_t payload_type;
    uint32_t clock_rate;
    read_payload(codecs, index, &payload_type, &clock_rate);
    if (payload_type > 127U || clock_rate == 0U)
      return LS200_STATUS_INVALID_DATA;
    for (prior = 0U; prior < index; ++prior) {
      uint8_t prior_payload_type;
      uint32_t prior_clock_rate;
      read_payload(codecs, prior, &prior_payload_type, &prior_clock_rate);
      if (prior_payload_type == payload_type && prior_clock_rate != clock_rate)
        return LS200_STATUS_INVALID_DATA;
    }
    stream->payload_types[index] = payload_type;
    stream->payload_clocks[index].payload_type = payload_type;
    stream->payload_clocks[index].clock_rate = clock_rate;
  }
  stream->payload_type_count = codec_count;
  return LS200_STATUS_OK;
}

ls200_status ls200_media_session_collect_payloads(
    const ls200_sdp_codec *codecs, size_t codec_count,
    ls200_media_session_stream *stream) {
  return collect_values(codecs, codec_count, stream, read_local);
}

ls200_status ls200_media_session_collect_negotiated_payloads(
    const ls200_sdp_negotiated_codec *codecs, size_t codec_count,
    ls200_media_session_stream *stream) {
  return collect_values(codecs, codec_count, stream, read_negotiated);
}
