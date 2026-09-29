#include "rtsp_private.h"
#include "aula_sipd/rtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

aula_status aula_rtsp_flush_request(aula_rtsp_gst_context *context) {
  ssize_t sent;
  if (context == NULL || context->pending_request_length == 0U ||
      context->pending_request_offset > context->pending_request_length) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (context->protocol_fixture != 0) {
    if (context->fixture_request_length != 0U ||
        context->pending_request_length > sizeof(context->fixture_request)) {
      return AULA_STATUS_STATE_ERROR;
    }
    (void)memcpy(context->fixture_request, context->pending_request,
                 context->pending_request_length);
    context->fixture_request_length = context->pending_request_length;
    context->pending_request_length = 0U;
    context->pending_request_offset = 0U;
    context->rtsp_state = context->pending_state;
    return AULA_STATUS_OK;
  }
  sent = send(context->rtsp_fd, context->pending_request + context->pending_request_offset,
              context->pending_request_length - context->pending_request_offset, 0);
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return AULA_STATUS_AGAIN;
  if (sent <= 0) return AULA_STATUS_IO_ERROR;
  context->pending_request_offset += (size_t)sent;
  if (context->pending_request_offset != context->pending_request_length) return AULA_STATUS_AGAIN;
  context->pending_request_length = 0U;
  context->pending_request_offset = 0U;
  context->rtsp_state = context->pending_state;
  return AULA_STATUS_OK;
}

static aula_status send_request(aula_rtsp_gst_context *context, const char *method,
                                 const char *request_uri, const char *extra_headers,
                                 aula_rtsp_session_state next_state) {
  int written;
  if (context == NULL || method == NULL || request_uri == NULL ||
      request_uri[0] == '\0' || extra_headers == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (context->pending_request_length != 0U) return aula_rtsp_flush_request(context);
  written = snprintf(context->pending_request, sizeof(context->pending_request),
                     "%s %s RTSP/1.0\r\nCSeq: %u\r\n%s\r\n", method,
                     request_uri, context->rtsp_cseq, extra_headers);
  if (written < 0 || (size_t)written >= sizeof(context->pending_request)) return AULA_STATUS_LIMIT_EXCEEDED;
  context->pending_request_length = (size_t)written;
  context->pending_request_offset = 0U;
  context->pending_state = next_state;
  context->rtsp_cseq++;
  return aula_rtsp_flush_request(context);
}

aula_status aula_rtsp_drive_request(aula_rtsp_gst_context *context) {
  int socket_error = 0;
  socklen_t length = sizeof(socket_error);
  if (context->rtsp_state == AULA_RTSP_DISABLED) return AULA_STATUS_AGAIN;
  if (context->rtsp_state != AULA_RTSP_CONNECTING) return AULA_STATUS_OK;
  if (getsockopt(context->rtsp_fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) return AULA_STATUS_IO_ERROR;
  if (socket_error == EINPROGRESS || socket_error == EALREADY) return AULA_STATUS_AGAIN;
  if (socket_error != 0) return AULA_STATUS_IO_ERROR;
  return send_request(context, "OPTIONS", context->video_uri.text, "",
                      AULA_RTSP_OPTIONS);
}

static int rtsp_sdp_line_next(aula_bytes body, size_t *cursor,
                              const uint8_t **out_line, size_t *out_length) {
  size_t start;
  size_t end;
  if (cursor == NULL || out_line == NULL || out_length == NULL ||
      body.data == NULL || *cursor >= body.length) {
    return 0;
  }
  start = *cursor;
  end = start;
  while (end < body.length && body.data[end] != '\n') end++;
  *cursor = end < body.length ? end + 1U : end;
  if (end > start && body.data[end - 1U] == '\r') end--;
  *out_line = body.data + start;
  *out_length = end - start;
  return 1;
}

static int rtsp_sdp_media_is_video(const uint8_t *line, size_t length) {
  return length >= sizeof("m=video") - 1U &&
      memcmp(line, "m=video", sizeof("m=video") - 1U) == 0 &&
      (length == sizeof("m=video") - 1U || line[sizeof("m=video") - 1U] == ' ' ||
       line[sizeof("m=video") - 1U] == '\t');
}

static int rtsp_sdp_prefix_is(const uint8_t *line, size_t length,
                              const char *prefix) {
  size_t prefix_length;
  if (line == NULL || prefix == NULL) return 0;
  prefix_length = strlen(prefix);
  if (length < prefix_length) return 0;
  return memcmp(line, prefix, prefix_length) == 0;
}

static int rtsp_sdp_parse_payload_type(const uint8_t *line, size_t length,
                                       size_t *cursor, uint8_t *out_payload_type) {
  size_t start;
  uint32_t value = 0U;
  if (line == NULL || cursor == NULL || out_payload_type == NULL) return 0;
  start = *cursor;
  while (*cursor < length && line[*cursor] >= '0' && line[*cursor] <= '9') {
    if (value > 12U) return 0;
    value = value * 10U + (uint32_t)(line[*cursor] - '0');
    (*cursor)++;
  }
  if (*cursor == start || value > 127U) return 0;
  *out_payload_type = (uint8_t)value;
  return 1;
}

static int rtsp_sdp_encoding_is_h264(const uint8_t *line, size_t length,
                                     size_t cursor) {
  static const char encoding[] = "H264/90000";
  size_t encoding_length = sizeof(encoding) - 1U;
  if (cursor + encoding_length > length) return 0;
  if (memcmp(line + cursor, encoding, encoding_length) != 0) return 0;
  if (cursor + encoding_length == length) return 1;
  return line[cursor + encoding_length] == ' ' ||
      line[cursor + encoding_length] == '\t';
}

static int rtsp_sdp_h264_payload_type(const uint8_t *line, size_t length,
                                      uint8_t *out_payload_type) {
  static const char prefix[] = "a=rtpmap:";
  size_t cursor = sizeof(prefix) - 1U;
  if (!rtsp_sdp_prefix_is(line, length, prefix)) return 0;
  if (!rtsp_sdp_parse_payload_type(line, length, &cursor, out_payload_type)) return 0;
  if (cursor >= length || line[cursor] != ' ') return 0;
  return rtsp_sdp_encoding_is_h264(line, length, cursor + 1U);
}

static int rtsp_sdp_control_component_safe(const uint8_t *value, size_t start,
                                           size_t end) {
  if (start == end) return 0;
  if (end - start == 1U && value[start] == '.') return 0;
  return end - start != 2U || value[start] != '.' || value[start + 1U] != '.';
}

static int rtsp_sdp_safe_control(const uint8_t *value, size_t length) {
  size_t index;
  size_t component_start = 0U;
  if (value == NULL || length == 0U || value[0] == '/' || value[0] == '.') return 0;
  for (index = 0U; index < length; ++index) {
    if (!aula_rtsp_path_character(value[index])) return 0;
    if (value[index] == '/') {
      if (!rtsp_sdp_control_component_safe(value, component_start, index)) return 0;
      component_start = index + 1U;
    }
  }
  return rtsp_sdp_control_component_safe(value, component_start, length);
}

static aula_status rtsp_build_setup_uri(aula_rtsp_gst_context *context,
                                         const uint8_t *control, size_t control_length) {
  size_t aggregate_length;
  int written;
  if (context == NULL || !rtsp_sdp_safe_control(control, control_length)) {
    return AULA_STATUS_INVALID_DATA;
  }
  aggregate_length = strlen(context->video_uri.text);
  if (aggregate_length == 0U) return AULA_STATUS_STATE_ERROR;
  written = snprintf(context->setup_uri.text, sizeof(context->setup_uri.text),
                     "%s%s%.*s", context->video_uri.text,
                     context->video_uri.text[aggregate_length - 1U] == '/' ? "" : "/",
                     (int)control_length, (const char *)control);
  if (written < 0 || (size_t)written >= sizeof(context->setup_uri.text)) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  (void)memcpy(context->setup_uri.address, context->video_uri.address,
               sizeof(context->setup_uri.address));
  context->setup_uri.port = context->video_uri.port;
  return AULA_STATUS_OK;
}

typedef struct rtsp_sdp_video_media {
  const uint8_t *control;
  size_t control_length;
  uint8_t payload_type;
  uint8_t declared_payload_types[128];
  int in_video;
  int h264_found;
  int control_found;
} rtsp_sdp_video_media;

static int rtsp_sdp_collect_declared_payloads(rtsp_sdp_video_media *media,
                                               const uint8_t *line,
                                               size_t line_length) {
  size_t cursor = sizeof("m=video ") - 1U;
  size_t field;
  size_t count = 0U;
  for (field = 0U; field < 2U; ++field) {
    size_t start = cursor;
    while (cursor < line_length && line[cursor] != ' ') cursor++;
    if (cursor == start || cursor == line_length) return 0;
    while (cursor < line_length && line[cursor] == ' ') cursor++;
  }
  while (cursor < line_length) {
    uint8_t payload_type;
    if (!rtsp_sdp_parse_payload_type(line, line_length, &cursor,
                                     &payload_type) ||
        media->declared_payload_types[payload_type] != 0U ||
        (cursor < line_length && line[cursor] != ' ')) return 0;
    media->declared_payload_types[payload_type] = 1U;
    count++;
    while (cursor < line_length && line[cursor] == ' ') cursor++;
  }
  return count != 0U;
}

static aula_status rtsp_sdp_begin_media(rtsp_sdp_video_media *media,
                                          const uint8_t *line,
                                          size_t line_length) {
  (void)memset(media, 0, sizeof(*media));
  media->in_video = rtsp_sdp_media_is_video(line, line_length);
  if (media->in_video != 0 &&
      !rtsp_sdp_collect_declared_payloads(media, line, line_length)) {
    return AULA_STATUS_INVALID_DATA;
  }
  return AULA_STATUS_OK;
}

static aula_status rtsp_sdp_collect_control(rtsp_sdp_video_media *media,
                                             const uint8_t *line,
                                             size_t line_length) {
  static const char control_prefix[] = "a=control:";
  size_t prefix_length = sizeof(control_prefix) - 1U;
  if (!rtsp_sdp_prefix_is(line, line_length, control_prefix)) return AULA_STATUS_OK;
  if (media->control_found != 0) return AULA_STATUS_INVALID_DATA;
  media->control = line + prefix_length;
  media->control_length = line_length - prefix_length;
  media->control_found = 1;
  return AULA_STATUS_OK;
}

static aula_status rtsp_sdp_collect_video_line(rtsp_sdp_video_media *media,
                                                const uint8_t *line,
                                                size_t line_length) {
  if (media->in_video == 0) return AULA_STATUS_OK;
  if (rtsp_sdp_h264_payload_type(line, line_length, &media->payload_type) != 0) {
    if (media->declared_payload_types[media->payload_type] == 0U) {
      return AULA_STATUS_INVALID_DATA;
    }
    media->h264_found = 1;
    return AULA_STATUS_OK;
  }
  return rtsp_sdp_collect_control(media, line, line_length);
}

static aula_status rtsp_sdp_finish_media(aula_rtsp_gst_context *context,
                                          const rtsp_sdp_video_media *media) {
  if (media->in_video == 0 || media->h264_found == 0) return AULA_STATUS_AGAIN;
  if (media->control_found == 0) return AULA_STATUS_UNSUPPORTED;
  context->h264_payload_type = media->payload_type;
  return rtsp_build_setup_uri(context, media->control, media->control_length);
}

static aula_status rtsp_find_h264_control(aula_rtsp_gst_context *context,
                                           aula_bytes body) {
  rtsp_sdp_video_media media;
  const uint8_t *line;
  size_t cursor = 0U;
  size_t line_length;
  aula_status status;
  if (context == NULL || body.data == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(&media, 0, sizeof(media));
  while (rtsp_sdp_line_next(body, &cursor, &line, &line_length) != 0) {
    if (line_length >= 2U && line[0] == 'm' && line[1] == '=') {
      status = rtsp_sdp_finish_media(context, &media);
      if (status != AULA_STATUS_AGAIN) return status;
      status = rtsp_sdp_begin_media(&media, line, line_length);
      if (status != AULA_STATUS_OK) return status;
      continue;
    }
    status = rtsp_sdp_collect_video_line(&media, line, line_length);
    if (status != AULA_STATUS_OK) return status;
  }
  status = rtsp_sdp_finish_media(context, &media);
  return status == AULA_STATUS_AGAIN ? AULA_STATUS_UNSUPPORTED : status;
}

static aula_status handle_response(aula_rtsp_gst_context *context, const aula_rtsp_message *message) {
  aula_status status;
  if (context->rtsp_cseq == 0U || message->cseq != context->rtsp_cseq - 1U) return AULA_STATUS_INVALID_DATA;
  if (message->status_code < 200U || message->status_code > 299U) { context->rtsp_state = AULA_RTSP_FAILED; return AULA_STATUS_IO_ERROR; }
  if (context->rtsp_state == AULA_RTSP_OPTIONS) {
    return send_request(context, "DESCRIBE", context->video_uri.text,
                        "Accept: application/sdp\r\n", AULA_RTSP_DESCRIBE);
  }
  if (context->rtsp_state == AULA_RTSP_DESCRIBE) {
    status = rtsp_find_h264_control(context, message->body);
    if (status != AULA_STATUS_OK) return status;
    return send_request(context, "SETUP", context->setup_uri.text,
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n",
                        AULA_RTSP_SETUP);
  }
  if (context->rtsp_state == AULA_RTSP_SETUP) {
    char headers[192];
    int written;
    if (message->session_id[0] == '\0') return AULA_STATUS_INVALID_DATA;
    written = snprintf(headers, sizeof(headers), "Session: %s\r\n", message->session_id);
    if (written < 0 || (size_t)written >= sizeof(headers)) return AULA_STATUS_LIMIT_EXCEEDED;
    return send_request(context, "PLAY", context->video_uri.text, headers,
                        AULA_RTSP_PLAY);
  }
  if (context->rtsp_state == AULA_RTSP_PLAY) { context->rtsp_state = AULA_RTSP_STREAMING; return AULA_STATUS_OK; }
  return AULA_STATUS_INVALID_DATA;
}

static aula_status finish_access_unit(aula_rtsp_gst_context *context, uint32_t timestamp) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  int has_idr = 0;
  size_t required;
  aula_status status;
  if (context->assembly_length == 0U ||
      aula_h264_annexb_iterator_init(&iterator, (aula_bytes){context->assembly, context->assembly_length},
                                      context->maximum_access_unit_bytes) != AULA_STATUS_OK) return AULA_STATUS_INVALID_DATA;
  for (;;) {
    status = aula_h264_annexb_iterator_next(&iterator, &nal);
    if (status == AULA_STATUS_END) break;
    if (status != AULA_STATUS_OK) return status;
    status = aula_h264_parameter_sets_update(&context->parameter_sets, &nal);
    if (status != AULA_STATUS_OK) return status;
    if (nal.is_idr != 0) has_idr = 1;
  }
  if (aula_h264_can_transmit_access_unit(&context->parameter_sets, has_idr) == 0) {
    context->health.frames_dropped++; context->assembly_length = 0U; return AULA_STATUS_AGAIN;
  }
  required = 8U + context->parameter_sets.sps_length + context->parameter_sets.pps_length + context->assembly_length;
  if (required > context->maximum_access_unit_bytes) return AULA_STATUS_LIMIT_EXCEEDED;
  context->video[0] = 0U; context->video[1] = 0U; context->video[2] = 0U; context->video[3] = 1U;
  (void)memcpy(context->video + 4U, context->parameter_sets.sps, context->parameter_sets.sps_length);
  context->video[4U + context->parameter_sets.sps_length] = 0U;
  context->video[5U + context->parameter_sets.sps_length] = 0U;
  context->video[6U + context->parameter_sets.sps_length] = 0U;
  context->video[7U + context->parameter_sets.sps_length] = 1U;
  (void)memcpy(context->video + 8U + context->parameter_sets.sps_length, context->parameter_sets.pps, context->parameter_sets.pps_length);
  (void)memcpy(context->video + 8U + context->parameter_sets.sps_length + context->parameter_sets.pps_length,
               context->assembly, context->assembly_length);
  context->video_length = required;
  context->video_pts_ns = ((uint64_t)timestamp * UINT64_C(1000000000)) / UINT64_C(90000);
  context->video_ready = 1; context->assembly_length = 0U;
  return AULA_STATUS_OK;
}

static aula_status handle_rtp(aula_rtsp_gst_context *context, aula_bytes bytes) {
  aula_rtp_packet packet;
  aula_mutable_bytes output;
  int complete = 0;
  aula_status status = aula_rtp_parse(bytes, &packet);
  if (status != AULA_STATUS_OK || packet.header.payload_type != context->h264_payload_type) return status == AULA_STATUS_OK ? AULA_STATUS_INVALID_DATA : status;
  output.data = context->assembly + context->assembly_length;
  output.capacity = context->maximum_access_unit_bytes - context->assembly_length;
  output.length = 0U;
  status = aula_h264_depacketizer_push(context->depacketizer, &packet, &output, &complete);
  if (status != AULA_STATUS_OK) return status;
  context->assembly_length += output.length;
  return complete == 0 ? AULA_STATUS_AGAIN : finish_access_unit(context, packet.header.timestamp);
}

aula_status aula_rtsp_consume_message(aula_rtsp_gst_context *context, const aula_rtsp_message *message) {
  if (message->kind == AULA_RTSP_MESSAGE_RESPONSE) return handle_response(context, message);
  if (context->rtsp_state != AULA_RTSP_STREAMING || message->channel != 0U) return AULA_STATUS_INVALID_DATA;
  return handle_rtp(context, message->body);
}

static aula_status fixture_take_request(aula_rtsp_gst_context *context,
                                         aula_mutable_bytes *out_request) {
  if (context == NULL || out_request == NULL ||
      (out_request->data == NULL && out_request->capacity != 0U)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  out_request->length = 0U;
  if (context->fixture_request_length == 0U) return AULA_STATUS_AGAIN;
  if (out_request->capacity < context->fixture_request_length) return AULA_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(out_request->data, context->fixture_request,
               context->fixture_request_length);
  out_request->length = context->fixture_request_length;
  context->fixture_request_length = 0U;
  return AULA_STATUS_OK;
}

static void fixture_set_frame(aula_rtsp_gst_context *context,
                              aula_media_frame *out_frame) {
  (void)memset(out_frame, 0, sizeof(*out_frame));
  out_frame->kind = AULA_MEDIA_VIDEO_H264_ANNEX_B;
  out_frame->data.data = context->video;
  out_frame->data.length = context->video_length;
  out_frame->pts_ns = context->video_pts_ns;
  out_frame->keyframe = 1;
  context->video_ready = 0;
}

aula_status aula_rtsp_protocol_fixture_create(uint32_t maximum_access_unit_bytes,
                                                aula_rtsp_protocol_fixture **out_fixture) {
  aula_rtsp_protocol_fixture *fixture;
  aula_status status;
  if (out_fixture == NULL || maximum_access_unit_bytes == 0U ||
      maximum_access_unit_bytes > AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  fixture = (aula_rtsp_protocol_fixture *)calloc(1U, sizeof(*fixture));
  if (fixture == NULL) return AULA_STATUS_INTERNAL_ERROR;
  fixture->context.assembly = (uint8_t *)malloc(maximum_access_unit_bytes);
  fixture->context.video = (uint8_t *)malloc(maximum_access_unit_bytes);
  status = aula_rtsp_stream_parser_create(&fixture->context.parser);
  if (status == AULA_STATUS_OK) {
    status = aula_h264_depacketizer_create(maximum_access_unit_bytes,
                                            &fixture->context.depacketizer);
  }
  if (fixture->context.assembly == NULL || fixture->context.video == NULL ||
      status != AULA_STATUS_OK) {
    aula_h264_depacketizer_destroy(fixture->context.depacketizer);
    aula_rtsp_stream_parser_destroy(fixture->context.parser);
    free(fixture->context.assembly);
    free(fixture->context.video);
    free(fixture);
    return status == AULA_STATUS_OK ? AULA_STATUS_INTERNAL_ERROR : status;
  }
  fixture->context.maximum_access_unit_bytes = maximum_access_unit_bytes;
  fixture->context.rtsp_fd = -1;
  fixture->context.protocol_fixture = 1;
  (void)memcpy(fixture->context.video_uri.text, "rtsp://127.0.0.1/fixture",
               sizeof("rtsp://127.0.0.1/fixture"));
  *out_fixture = fixture;
  return AULA_STATUS_OK;
}

aula_status aula_rtsp_protocol_fixture_start(aula_rtsp_protocol_fixture *fixture,
                                               aula_mutable_bytes *out_request) {
  aula_rtsp_gst_context *context = fixture == NULL ? NULL : &fixture->context;
  aula_status status;
  if (context == NULL || context->opened != 0 || out_request == NULL) {
    return AULA_STATUS_STATE_ERROR;
  }
  context->opened = 1;
  context->started = 1;
  context->rtsp_cseq = 1U;
  context->rtsp_state = AULA_RTSP_CONNECTING;
  status = send_request(context, "OPTIONS", context->video_uri.text, "",
                        AULA_RTSP_OPTIONS);
  if (status != AULA_STATUS_OK) return status;
  return fixture_take_request(context, out_request);
}

aula_status aula_rtsp_protocol_fixture_push(aula_rtsp_protocol_fixture *fixture,
                                              aula_bytes input,
                                              aula_mutable_bytes *out_request,
                                              aula_media_frame *out_frame) {
  aula_rtsp_gst_context *context = fixture == NULL ? NULL : &fixture->context;
  aula_rtsp_message message;
  aula_status status;
  if (context == NULL || out_request == NULL || out_frame == NULL ||
      context->started == 0 || context->rtsp_state == AULA_RTSP_FAILED ||
      (input.data == NULL && input.length != 0U)) return AULA_STATUS_STATE_ERROR;
  out_request->length = 0U;
  (void)memset(out_frame, 0, sizeof(*out_frame));
  status = aula_rtsp_stream_parser_push(context->parser, input, &message);
  while (status == AULA_STATUS_OK) {
    status = aula_rtsp_consume_message(context, &message);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) {
      context->rtsp_state = AULA_RTSP_FAILED;
      return status;
    }
    if (context->fixture_request_length != 0U) {
      return fixture_take_request(context, out_request);
    }
    if (context->video_ready != 0) {
      fixture_set_frame(context, out_frame);
      return AULA_STATUS_OK;
    }
    status = aula_rtsp_stream_parser_push(context->parser,
                                           (aula_bytes){NULL, 0U}, &message);
  }
  return status;
}

aula_status aula_rtsp_protocol_fixture_eof(aula_rtsp_protocol_fixture *fixture) {
  aula_rtsp_gst_context *context = fixture == NULL ? NULL : &fixture->context;
  if (context == NULL || context->started == 0) return AULA_STATUS_STATE_ERROR;
  context->pending_request_length = 0U;
  context->pending_request_offset = 0U;
  context->fixture_request_length = 0U;
  context->rtsp_state = AULA_RTSP_FAILED;
  return AULA_STATUS_END;
}

void aula_rtsp_protocol_fixture_destroy(aula_rtsp_protocol_fixture *fixture) {
  if (fixture == NULL) return;
  aula_h264_depacketizer_destroy(fixture->context.depacketizer);
  aula_rtsp_stream_parser_destroy(fixture->context.parser);
  free(fixture->context.assembly);
  free(fixture->context.video);
  free(fixture);
}
