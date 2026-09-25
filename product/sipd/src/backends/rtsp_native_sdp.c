#include "rtsp_private.h"
#include "../media/h264_profile_private.h"

#include <stdio.h>
#include <string.h>

typedef enum native_sdp_media_kind {
  NATIVE_SDP_NONE = 0,
  NATIVE_SDP_VIDEO,
  NATIVE_SDP_AUDIO
} native_sdp_media_kind;

typedef struct native_sdp_media {
  native_sdp_media_kind kind;
  const uint8_t *control;
  size_t control_length;
  uint8_t payload_type;
  uint32_t clock_rate;
  uint8_t channels;
  ls200_aac_config aac;
  ls200_rtsp_h264_capability h264;
  uint8_t declared_payload_types[128];
  uint8_t fmtp_payload_type;
  int control_seen;
  int codec_seen;
  int config_seen;
  int h264_fmtp_seen;
} native_sdp_media;

static int native_sdp_decimal(const uint8_t *line, size_t length,
                              size_t *cursor, uint32_t maximum,
                              uint32_t *out_value);

static int native_sdp_skip_field(const uint8_t *line, size_t length,
                                 size_t *cursor) {
  size_t start = *cursor;
  while (*cursor < length && line[*cursor] != ' ') (*cursor)++;
  if (*cursor == start || *cursor == length) return 0;
  while (*cursor < length && line[*cursor] == ' ') (*cursor)++;
  return *cursor < length;
}

static int native_sdp_record_declared_payload(native_sdp_media *media,
                                                const uint8_t *line,
                                                size_t length,
                                                size_t *cursor) {
  uint32_t payload_type;
  if (!native_sdp_decimal(line, length, cursor, 127U, &payload_type) ||
      media->declared_payload_types[payload_type] != 0U ||
      (*cursor < length && line[*cursor] != ' ')) return 0;
  media->declared_payload_types[payload_type] = 1U;
  while (*cursor < length && line[*cursor] == ' ') (*cursor)++;
  return 1;
}

static int native_sdp_declared_payloads(native_sdp_media *media,
                                         const uint8_t *line, size_t length) {
  size_t cursor = media->kind == NATIVE_SDP_VIDEO ?
      sizeof("m=video ") - 1U : sizeof("m=audio ") - 1U;
  size_t count = 0U;
  if (!native_sdp_skip_field(line, length, &cursor) ||
      !native_sdp_skip_field(line, length, &cursor)) return 0;
  while (cursor < length) {
    if (!native_sdp_record_declared_payload(media, line, length, &cursor)) return 0;
    count++;
  }
  return count != 0U;
}

static int native_sdp_line_next(ls200_bytes sdp, size_t *cursor,
                                const uint8_t **out_line, size_t *out_length) {
  size_t start;
  size_t end;
  if (cursor == NULL || out_line == NULL || out_length == NULL ||
      sdp.data == NULL || *cursor >= sdp.length) return 0;
  start = *cursor;
  end = start;
  while (end < sdp.length && sdp.data[end] != '\n') end++;
  *cursor = end < sdp.length ? end + 1U : end;
  if (end > start && sdp.data[end - 1U] == '\r') end--;
  *out_line = sdp.data + start;
  *out_length = end - start;
  return 1;
}

static int native_sdp_prefix(const uint8_t *line, size_t length,
                             const char *prefix) {
  size_t prefix_length = strlen(prefix);
  return line != NULL && length >= prefix_length &&
      memcmp(line, prefix, prefix_length) == 0;
}

static int native_sdp_decimal(const uint8_t *line, size_t length,
                              size_t *cursor, uint32_t maximum,
                              uint32_t *out_value) {
  uint32_t value = 0U;
  size_t start;
  if (line == NULL || cursor == NULL || out_value == NULL) return 0;
  start = *cursor;
  while (*cursor < length && line[*cursor] >= '0' && line[*cursor] <= '9') {
    uint32_t digit = (uint32_t)(line[*cursor] - '0');
    if (value > (maximum - digit) / 10U) return 0;
    value = value * 10U + digit;
    (*cursor)++;
  }
  if (*cursor == start) return 0;
  *out_value = value;
  return 1;
}

static native_sdp_media_kind native_sdp_media_kind_from_line(
    const uint8_t *line, size_t length) {
  if (native_sdp_prefix(line, length, "m=video ")) return NATIVE_SDP_VIDEO;
  if (native_sdp_prefix(line, length, "m=audio ")) return NATIVE_SDP_AUDIO;
  return NATIVE_SDP_NONE;
}

static int native_sdp_path_component_safe(const uint8_t *value, size_t start,
                                          size_t end) {
  if (start == end || (end - start == 1U && value[start] == '.')) return 0;
  return end - start != 2U || value[start] != '.' || value[start + 1U] != '.';
}

static int native_sdp_control_safe(const uint8_t *value, size_t length) {
  size_t index;
  size_t component_start = 0U;
  if (value == NULL || length == 0U || value[0] == '/' || value[0] == '.') return 0;
  for (index = 0U; index < length; ++index) {
    if (!ls200_rtsp_path_character(value[index])) return 0;
    if (value[index] == '/') {
      if (!native_sdp_path_component_safe(value, component_start, index)) return 0;
      component_start = index + 1U;
    }
  }
  return native_sdp_path_component_safe(value, component_start, length);
}

static ls200_status native_sdp_track_uri(const ls200_rtsp_uri *aggregate_uri,
                                         const uint8_t *control,
                                         size_t control_length,
                                         ls200_rtsp_uri *out_uri) {
  size_t aggregate_length;
  int written;
  if (aggregate_uri == NULL || out_uri == NULL ||
      !native_sdp_control_safe(control, control_length)) return LS200_STATUS_INVALID_DATA;
  aggregate_length = strlen(aggregate_uri->text);
  if (aggregate_length == 0U) return LS200_STATUS_STATE_ERROR;
  written = snprintf(out_uri->text, sizeof(out_uri->text), "%s%s%.*s",
                     aggregate_uri->text,
                     aggregate_uri->text[aggregate_length - 1U] == '/' ? "" : "/",
                     (int)control_length, (const char *)control);
  if (written < 0 || (size_t)written >= sizeof(out_uri->text)) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  (void)memcpy(out_uri->address, aggregate_uri->address, sizeof(out_uri->address));
  out_uri->port = aggregate_uri->port;
  return LS200_STATUS_OK;
}

static int native_sdp_codec(const uint8_t *line, size_t length, size_t cursor,
                            const char *codec, uint32_t *out_rate,
                            uint8_t *out_channels) {
  size_t codec_length = strlen(codec);
  uint32_t rate;
  uint32_t channels = 1U;
  if (cursor + codec_length > length ||
      memcmp(line + cursor, codec, codec_length) != 0 ||
      cursor + codec_length >= length || line[cursor + codec_length] != '/') {
    return 0;
  }
  cursor += codec_length + 1U;
  if (!native_sdp_decimal(line, length, &cursor, 192000U, &rate) || rate == 0U) return 0;
  if (cursor < length && line[cursor] == '/') {
    cursor++;
    if (!native_sdp_decimal(line, length, &cursor, 2U, &channels) || channels == 0U) return 0;
  }
  if (cursor != length || out_rate == NULL || out_channels == NULL) return 0;
  *out_rate = rate;
  *out_channels = (uint8_t)channels;
  return 1;
}

static ls200_status native_sdp_select_video_codec(native_sdp_media *media,
                                                    uint32_t payload_type,
                                                    uint32_t rate,
                                                    uint8_t channels) {
  if (media->codec_seen != 0 ||
      media->declared_payload_types[payload_type] == 0U ||
      rate != 90000U || channels != 1U) return LS200_STATUS_INVALID_DATA;
  media->payload_type = (uint8_t)payload_type;
  media->clock_rate = rate;
  media->codec_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_select_audio_codec(native_sdp_media *media,
                                                    uint32_t payload_type,
                                                    uint32_t rate,
                                                    uint8_t channels) {
  if (media->codec_seen != 0 ||
      media->declared_payload_types[payload_type] == 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  media->payload_type = (uint8_t)payload_type;
  media->clock_rate = rate;
  media->channels = channels;
  media->codec_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_rtpmap(native_sdp_media *media,
                                      const uint8_t *line, size_t length) {
  static const char prefix[] = "a=rtpmap:";
  uint32_t payload_type;
  uint32_t rate;
  uint8_t channels;
  size_t cursor = sizeof(prefix) - 1U;
  if (media == NULL || !native_sdp_prefix(line, length, prefix)) return LS200_STATUS_OK;
  if (!native_sdp_decimal(line, length, &cursor, 127U, &payload_type) ||
      cursor >= length || line[cursor] != ' ') return LS200_STATUS_INVALID_DATA;
  cursor++;
  if (media->kind == NATIVE_SDP_VIDEO &&
      native_sdp_codec(line, length, cursor, "H264", &rate, &channels)) {
    return native_sdp_select_video_codec(media, payload_type, rate, channels);
  } else if (media->kind == NATIVE_SDP_AUDIO &&
             native_sdp_codec(line, length, cursor, "MPEG4-GENERIC", &rate, &channels)) {
    return native_sdp_select_audio_codec(media, payload_type, rate, channels);
  }
  return LS200_STATUS_OK;
}

static int native_sdp_hex_nibble(uint8_t value, uint8_t *out_nibble) {
  if (value >= '0' && value <= '9') { *out_nibble = (uint8_t)(value - '0'); return 1; }
  if (value >= 'a' && value <= 'f') { *out_nibble = (uint8_t)(value - 'a' + 10U); return 1; }
  if (value >= 'A' && value <= 'F') { *out_nibble = (uint8_t)(value - 'A' + 10U); return 1; }
  return 0;
}

static ls200_status native_sdp_parse_asc_hex(const uint8_t *value,
                                              size_t length,
                                              uint8_t out_asc[2]) {
  uint8_t nibbles[4];
  size_t index;
  if (value == NULL || out_asc == NULL || length != sizeof(nibbles)) {
    return LS200_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < sizeof(nibbles); ++index) {
    if (!native_sdp_hex_nibble(value[index], &nibbles[index])) {
      return LS200_STATUS_INVALID_DATA;
    }
  }
  out_asc[0] = (uint8_t)((nibbles[0] << 4U) | nibbles[1]);
  out_asc[1] = (uint8_t)((nibbles[2] << 4U) | nibbles[3]);
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_find_fmtp_config(const uint8_t *line,
                                                 size_t length, size_t cursor,
                                                 const uint8_t **out_value,
                                                 size_t *out_length) {
  static const char config_name[] = "config=";
  size_t start;
  if (line == NULL || out_value == NULL || out_length == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  while (cursor + sizeof(config_name) - 1U <= length &&
         memcmp(line + cursor, config_name, sizeof(config_name) - 1U) != 0) cursor++;
  if (cursor + sizeof(config_name) - 1U > length) return LS200_STATUS_INVALID_DATA;
  start = cursor + sizeof(config_name) - 1U;
  cursor = start;
  while (cursor < length && line[cursor] != ';' && line[cursor] != ' ' &&
         line[cursor] != '\t') cursor++;
  *out_value = line + start;
  *out_length = cursor - start;
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_aac_config(native_sdp_media *media,
                                           const uint8_t *line, size_t length) {
  static const char prefix[] = "a=fmtp:";
  uint32_t payload_type;
  size_t cursor = sizeof(prefix) - 1U;
  size_t config_length;
  const uint8_t *config_value;
  uint8_t asc[2];
  if (media == NULL || media->kind != NATIVE_SDP_AUDIO ||
      !native_sdp_prefix(line, length, prefix)) return LS200_STATUS_OK;
  if (!native_sdp_decimal(line, length, &cursor, 127U, &payload_type) ||
      cursor >= length || line[cursor] != ' ' || !media->codec_seen ||
      payload_type != media->payload_type) return LS200_STATUS_OK;
  if (media->config_seen != 0 || native_sdp_find_fmtp_config(
      line, length, cursor + 1U, &config_value, &config_length) != LS200_STATUS_OK ||
      native_sdp_parse_asc_hex(config_value, config_length, asc) !=
      LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  if (ls200_aac_parse_audio_specific_config((ls200_bytes){asc, sizeof(asc)},
                                            &media->aac) != LS200_STATUS_OK) {
    return LS200_STATUS_UNSUPPORTED;
  }
  media->config_seen = 1;
  return LS200_STATUS_OK;
}

static int native_sdp_field_equals(const uint8_t *field, size_t length,
                                   const char *name) {
  size_t name_length = strlen(name);
  return length == name_length && memcmp(field, name, name_length) == 0;
}

static size_t native_sdp_skip_fmtp_separators(
    const uint8_t *line, size_t length, size_t cursor) {
  while (cursor < length && (line[cursor] == ' ' || line[cursor] == '\t' ||
                             line[cursor] == ';')) cursor++;
  return cursor;
}

static ls200_status native_sdp_next_fmtp_field(
    const uint8_t *line, size_t length, size_t *cursor,
    ls200_bytes *name, ls200_bytes *value) {
  size_t name_start;
  size_t name_end;
  size_t value_start;
  *cursor = native_sdp_skip_fmtp_separators(line, length, *cursor);
  if (*cursor == length) return LS200_STATUS_END;
  name_start = *cursor;
  while (*cursor < length && line[*cursor] != '=' && line[*cursor] != ';')
    (*cursor)++;
  if (*cursor == length || line[*cursor] != '=') return LS200_STATUS_INVALID_DATA;
  name_end = (*cursor)++;
  value_start = *cursor;
  while (*cursor < length && line[*cursor] != ';' && line[*cursor] != ' ' &&
         line[*cursor] != '\t') (*cursor)++;
  if (name_end == name_start || *cursor == value_start)
    return LS200_STATUS_INVALID_DATA;
  *name = (ls200_bytes){line + name_start, name_end - name_start};
  *value = (ls200_bytes){line + value_start, *cursor - value_start};
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_h264_fmtp_fields(
    native_sdp_media *media, const uint8_t *line, size_t length,
    size_t cursor) {
  int profile_seen = 0;
  while (cursor < length) {
    ls200_bytes name;
    ls200_bytes value;
    ls200_status status = native_sdp_next_fmtp_field(
        line, length, &cursor, &name, &value);
    if (status == LS200_STATUS_END) break;
    if (status != LS200_STATUS_OK) return status;
    if (native_sdp_field_equals(name.data, name.length, "profile-level-id")) {
      if (profile_seen != 0 || value.length != 6U)
        return LS200_STATUS_INVALID_DATA;
      (void)memcpy(media->h264.profile_level_id, value.data, 6U);
      media->h264.profile_level_id[6] = '\0';
      if (!ls200_h264_profile_level_id_valid(media->h264.profile_level_id))
        return LS200_STATUS_UNSUPPORTED;
      profile_seen = 1;
    }
  }
  return profile_seen != 0 ? LS200_STATUS_OK : LS200_STATUS_UNSUPPORTED;
}

static ls200_status native_sdp_h264_fmtp(native_sdp_media *media,
                                          const uint8_t *line,
                                          size_t length) {
  static const char prefix[] = "a=fmtp:";
  uint32_t payload_type;
  size_t cursor = sizeof(prefix) - 1U;
  ls200_status status;
  if (media == NULL || media->kind != NATIVE_SDP_VIDEO ||
      !native_sdp_prefix(line, length, prefix)) return LS200_STATUS_OK;
  if (!native_sdp_decimal(line, length, &cursor, 127U, &payload_type) ||
      cursor >= length || line[cursor] != ' ' ||
      media->declared_payload_types[payload_type] == 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (media->h264_fmtp_seen != 0) return LS200_STATUS_INVALID_DATA;
  status = native_sdp_h264_fmtp_fields(media, line, length, cursor + 1U);
  if (status != LS200_STATUS_OK) return status;
  media->fmtp_payload_type = (uint8_t)payload_type;
  media->h264_fmtp_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_control(native_sdp_media *media,
                                       const uint8_t *line, size_t length) {
  static const char prefix[] = "a=control:";
  size_t prefix_length = sizeof(prefix) - 1U;
  if (media == NULL || !native_sdp_prefix(line, length, prefix)) return LS200_STATUS_OK;
  if (media->control_seen != 0) return LS200_STATUS_INVALID_DATA;
  media->control = line + prefix_length;
  media->control_length = length - prefix_length;
  media->control_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_collect(native_sdp_media *media,
                                       const uint8_t *line, size_t length) {
  ls200_status status;
  if (media == NULL || media->kind == NATIVE_SDP_NONE) return LS200_STATUS_OK;
  status = native_sdp_rtpmap(media, line, length);
  if (status != LS200_STATUS_OK) return status;
  status = native_sdp_aac_config(media, line, length);
  if (status != LS200_STATUS_OK) return status;
  status = native_sdp_h264_fmtp(media, line, length);
  if (status != LS200_STATUS_OK) return status;
  return native_sdp_control(media, line, length);
}

static ls200_status native_sdp_finish_video(const ls200_rtsp_uri *aggregate_uri,
                                            const native_sdp_media *media,
                                            ls200_rtsp_native_tracks *tracks,
                                            int *video_found) {
  if (media->codec_seen == 0 || media->control_seen == 0)
    return LS200_STATUS_UNSUPPORTED;
  if (media->h264_fmtp_seen != 0 &&
      media->fmtp_payload_type != media->payload_type)
    return LS200_STATUS_UNSUPPORTED;
  if (*video_found != 0) return LS200_STATUS_INVALID_DATA;
  *video_found = 1;
  tracks->h264_payload_type = media->payload_type;
  tracks->h264 = media->h264;
  return native_sdp_track_uri(aggregate_uri, media->control, media->control_length,
                              &tracks->video_uri);
}

static ls200_status native_sdp_finish_audio(const ls200_rtsp_uri *aggregate_uri,
                                            const native_sdp_media *media,
                                            ls200_rtsp_native_tracks *tracks,
                                            int *audio_found) {
  if (media->codec_seen == 0 || media->config_seen == 0 || media->control_seen == 0 ||
      media->clock_rate != media->aac.sample_rate || media->channels != media->aac.channels) {
    return LS200_STATUS_UNSUPPORTED;
  }
  if (*audio_found != 0) return LS200_STATUS_INVALID_DATA;
  *audio_found = 1;
  tracks->aac_payload_type = media->payload_type;
  tracks->aac = media->aac;
  return native_sdp_track_uri(aggregate_uri, media->control, media->control_length,
                              &tracks->audio_uri);
}

static ls200_status native_sdp_finish(const ls200_rtsp_uri *aggregate_uri,
                                      const native_sdp_media *media,
                                      ls200_rtsp_native_tracks *tracks,
                                      int *video_found, int *audio_found) {
  if (aggregate_uri == NULL || media == NULL || tracks == NULL ||
      video_found == NULL || audio_found == NULL || media->kind == NATIVE_SDP_NONE) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  return media->kind == NATIVE_SDP_VIDEO ?
      native_sdp_finish_video(aggregate_uri, media, tracks, video_found) :
      native_sdp_finish_audio(aggregate_uri, media, tracks, audio_found);
}

static ls200_status native_sdp_start_media(const ls200_rtsp_uri *aggregate_uri,
                                           native_sdp_media *media,
                                           const uint8_t *line, size_t length,
                                           ls200_rtsp_native_tracks *tracks,
                                           int *video_found, int *audio_found) {
  ls200_status status = native_sdp_finish(aggregate_uri, media, tracks,
                                          video_found, audio_found);
  if (status != LS200_STATUS_OK && media->kind != NATIVE_SDP_NONE) return status;
  (void)memset(media, 0, sizeof(*media));
  media->kind = native_sdp_media_kind_from_line(line, length);
  if (media->kind != NATIVE_SDP_NONE &&
      !native_sdp_declared_payloads(media, line, length)) {
    return LS200_STATUS_INVALID_DATA;
  }
  return LS200_STATUS_OK;
}

static ls200_status native_sdp_process_line(const ls200_rtsp_uri *aggregate_uri,
                                            native_sdp_media *media,
                                            const uint8_t *line, size_t length,
                                            ls200_rtsp_native_tracks *tracks,
                                            int *video_found, int *audio_found) {
  if (length >= 2U && line[0] == 'm' && line[1] == '=') {
    return native_sdp_start_media(aggregate_uri, media, line, length, tracks,
                                  video_found, audio_found);
  }
  return native_sdp_collect(media, line, length);
}

static int native_sdp_inputs_valid(const ls200_rtsp_uri *aggregate_uri,
                                   ls200_bytes sdp,
                                   const ls200_rtsp_native_tracks *out_tracks) {
  return aggregate_uri != NULL && sdp.data != NULL && sdp.length != 0U &&
      sdp.length <= LS200_RTSP_MAX_BODY_BYTES && out_tracks != NULL;
}

ls200_status ls200_rtsp_sdp_select_native_tracks(
    const ls200_rtsp_uri *aggregate_uri, ls200_bytes sdp,
    ls200_rtsp_native_tracks *out_tracks) {
  native_sdp_media media;
  const uint8_t *line;
  size_t line_length;
  size_t cursor = 0U;
  int video_found = 0;
  int audio_found = 0;
  ls200_status status;
  if (!native_sdp_inputs_valid(aggregate_uri, sdp, out_tracks)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(out_tracks, 0, sizeof(*out_tracks));
  (void)memset(&media, 0, sizeof(media));
  while (native_sdp_line_next(sdp, &cursor, &line, &line_length) != 0) {
    status = native_sdp_process_line(aggregate_uri, &media, line, line_length,
                                     out_tracks, &video_found, &audio_found);
    if (status != LS200_STATUS_OK) return status;
  }
  status = native_sdp_finish(aggregate_uri, &media, out_tracks,
                             &video_found, &audio_found);
  if (status != LS200_STATUS_OK && media.kind != NATIVE_SDP_NONE) return status;
  return video_found != 0 && audio_found != 0 ? LS200_STATUS_OK : LS200_STATUS_UNSUPPORTED;
}
