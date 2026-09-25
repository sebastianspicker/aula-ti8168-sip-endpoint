#include "preview_reader_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct preview_video_selection {
  const uint8_t *control;
  size_t control_length;
  uint8_t payload_type;
  int in_video;
  int h264_found;
  int control_found;
  int selected;
} preview_video_selection;

ls200_status ls200_preview_render_request(
    ls200_preview_reader *reader, const char *method, const char *uri,
    const char *extra_headers, ls200_preview_reader_state next_state,
    ls200_mutable_bytes *output) {
  int written;
  if (reader == NULL || method == NULL || uri == NULL || extra_headers == NULL ||
      !ls200_preview_output_is_valid(output)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  written = snprintf((char *)output->data, output->capacity,
                     "%s %s RTSP/1.0\r\nCSeq: %u\r\n"
                     "User-Agent: ls200-console-preview/1\r\n%s\r\n",
                     method, uri, reader->next_cseq, extra_headers);
  if (written < 0 || (size_t)written >= output->capacity) {
    output->length = 0U;
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  output->length = (size_t)written;
  reader->next_cseq++;
  reader->state = next_state;
  return LS200_STATUS_OK;
}

static int line_prefix_is(const uint8_t *line, size_t length,
                          const char *prefix) {
  size_t prefix_length = strlen(prefix);
  return length >= prefix_length && memcmp(line, prefix, prefix_length) == 0;
}

static int next_sdp_line(ls200_bytes body, size_t *cursor,
                         const uint8_t **out_line, size_t *out_length) {
  const uint8_t *newline;
  size_t remaining;
  if (body.data == NULL || cursor == NULL || out_line == NULL ||
      out_length == NULL || *cursor >= body.length) {
    return 0;
  }
  *out_line = body.data + *cursor;
  remaining = body.length - *cursor;
  newline = memchr(*out_line, '\n', remaining);
  *out_length = newline == NULL ? remaining : (size_t)(newline - *out_line);
  *cursor += *out_length + (newline == NULL ? 0U : 1U);
  if (*out_length > 0U && (*out_line)[*out_length - 1U] == '\r') {
    --*out_length;
  }
  return 1;
}

static int control_character_is_safe(unsigned char value) {
  if (value >= 'a' && value <= 'z') return 1;
  if (value >= 'A' && value <= 'Z') return 1;
  if (value >= '0' && value <= '9') return 1;
  return value == '_' || value == '-' || value == '.';
}

static int control_component_is_safe(const uint8_t *value, size_t start,
                                     size_t end) {
  size_t index;
  if (end == start || (end - start == 1U && value[start] == '.') ||
      (end - start == 2U && value[start] == '.' && value[start + 1U] == '.')) {
    return 0;
  }
  for (index = start; index < end; ++index) {
    if (!control_character_is_safe(value[index])) return 0;
  }
  return 1;
}

static int control_is_safe(const uint8_t *value, size_t length) {
  size_t index;
  size_t component = 0U;
  if (value == NULL || length == 0U || length > 96U || value[0] == '/' ||
      value[0] == '.') {
    return 0;
  }
  for (index = 0U; index < length; ++index) {
    if (value[index] == '/') {
      if (!control_component_is_safe(value, component, index)) return 0;
      component = index + 1U;
    }
  }
  return control_component_is_safe(value, component, length);
}

static int parse_h264_rtpmap(const uint8_t *value, size_t length,
                             uint8_t *out_payload_type) {
  size_t index = 0U;
  unsigned int result = 0U;
  if (value == NULL || out_payload_type == NULL || length == 0U) return 0;
  while (index < length && value[index] >= '0' && value[index] <= '9') {
    result = result * 10U + (unsigned int)(value[index] - '0');
    if (result > 127U) return 0;
    index++;
  }
  if (index == 0U || index >= length || value[index++] != ' ') return 0;
  if (length - index != sizeof("H264/90000") - 1U ||
      memcmp(value + index, "H264/90000", sizeof("H264/90000") - 1U) != 0) {
    return 0;
  }
  *out_payload_type = (uint8_t)result;
  return 1;
}

static ls200_status finish_video_selection(ls200_preview_reader *reader,
                                           preview_video_selection *selection) {
  int written;
  if (selection->in_video == 0) return LS200_STATUS_AGAIN;
  if (selection->h264_found == 0 && selection->control_found == 0) {
    return LS200_STATUS_AGAIN;
  }
  if (selection->h264_found == 0 || selection->control_found == 0) {
    return LS200_STATUS_UNSUPPORTED;
  }
  if (selection->selected != 0 ||
      !control_is_safe(selection->control, selection->control_length)) {
    return LS200_STATUS_INVALID_DATA;
  }
  reader->payload_type = selection->payload_type;
  written = snprintf(reader->setup_uri, sizeof(reader->setup_uri), "%s/%.*s",
                     reader->aggregate_uri, (int)selection->control_length,
                     (const char *)selection->control);
  if (written < 0 || (size_t)written >= sizeof(reader->setup_uri)) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  selection->selected = 1;
  return LS200_STATUS_OK;
}

static ls200_status collect_video_line(preview_video_selection *selection,
                                       const uint8_t *line,
                                       size_t line_length) {
  static const char control_prefix[] = "a=control:";
  static const char rtpmap_prefix[] = "a=rtpmap:";
  uint8_t candidate;
  if (selection->in_video == 0) return LS200_STATUS_OK;
  if (line_prefix_is(line, line_length, rtpmap_prefix) &&
      parse_h264_rtpmap(line + sizeof(rtpmap_prefix) - 1U,
                        line_length - (sizeof(rtpmap_prefix) - 1U),
                        &candidate)) {
    if (selection->h264_found != 0) return LS200_STATUS_INVALID_DATA;
    selection->payload_type = candidate;
    selection->h264_found = 1;
  } else if (line_prefix_is(line, line_length, control_prefix)) {
    if (selection->control_found != 0) return LS200_STATUS_INVALID_DATA;
    selection->control = line + sizeof(control_prefix) - 1U;
    selection->control_length = line_length - (sizeof(control_prefix) - 1U);
    selection->control_found = 1;
  }
  return LS200_STATUS_OK;
}

static ls200_status select_video_track(ls200_preview_reader *reader,
                                       ls200_bytes sdp) {
  preview_video_selection selection;
  const uint8_t *line;
  size_t cursor = 0U;
  size_t line_length;
  ls200_status status;
  if (reader == NULL || sdp.data == NULL || sdp.length == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(&selection, 0, sizeof(selection));
  while (next_sdp_line(sdp, &cursor, &line, &line_length) != 0) {
    if (line_prefix_is(line, line_length, "m=")) {
      status = finish_video_selection(reader, &selection);
      if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
      selection.in_video = line_prefix_is(line, line_length, "m=video ");
      selection.h264_found = 0;
      selection.control_found = 0;
      selection.control = NULL;
      selection.control_length = 0U;
    } else {
      status = collect_video_line(&selection, line, line_length);
      if (status != LS200_STATUS_OK) return status;
    }
  }
  status = finish_video_selection(reader, &selection);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  return selection.selected != 0 ? LS200_STATUS_OK : LS200_STATUS_UNSUPPORTED;
}

ls200_status ls200_preview_consume_response(
    ls200_preview_reader *reader, const ls200_rtsp_message *message,
    ls200_mutable_bytes *out_request) {
  ls200_status status;
  if (message->cseq + 1U != reader->next_cseq || message->status_code < 200U ||
      message->status_code > 299U) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (reader->state == LS200_PREVIEW_READER_OPTIONS) {
    return ls200_preview_render_request(
        reader, "DESCRIBE", reader->aggregate_uri,
        "Accept: application/sdp\r\n", LS200_PREVIEW_READER_DESCRIBE,
        out_request);
  }
  if (reader->state == LS200_PREVIEW_READER_DESCRIBE) {
    status = select_video_track(reader, message->body);
    if (status != LS200_STATUS_OK) return status;
    return ls200_preview_render_request(
        reader, "SETUP", reader->setup_uri,
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n",
        LS200_PREVIEW_READER_SETUP, out_request);
  }
  if (reader->state == LS200_PREVIEW_READER_SETUP) {
    char session_header[160];
    int written;
    if (message->session_id[0] == '\0') return LS200_STATUS_INVALID_DATA;
    written = snprintf(session_header, sizeof(session_header),
                       "Session: %s\r\n", message->session_id);
    if (written < 0 || (size_t)written >= sizeof(session_header)) {
      return LS200_STATUS_LIMIT_EXCEEDED;
    }
    return ls200_preview_render_request(
        reader, "PLAY", reader->aggregate_uri, session_header,
        LS200_PREVIEW_READER_PLAY, out_request);
  }
  if (reader->state == LS200_PREVIEW_READER_PLAY) {
    reader->state = LS200_PREVIEW_READER_STREAMING;
    return LS200_STATUS_AGAIN;
  }
  return LS200_STATUS_STATE_ERROR;
}
