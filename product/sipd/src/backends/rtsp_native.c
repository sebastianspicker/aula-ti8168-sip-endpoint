#include "rtsp_private.h"
#include "aula_sipd/media_aac.h"
#include "../media/h264_profile_private.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static aula_status native_describe(native_context *c, aula_bytes sdp) {
  aula_rtsp_native_tracks tracks;
  char headers[256];
  aula_status status;
  if (c == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_rtsp_sdp_select_native_tracks(&c->aggregate_uri, sdp, &tracks);
  if (status != AULA_STATUS_OK) return status;
  if (c->expected_h264_present != 0 &&
      tracks.h264.profile_level_id[0] != '\0' &&
      !aula_h264_profile_level_id_compatible(
          c->expected_h264.profile_level_id,
          tracks.h264.profile_level_id)) {
    return AULA_STATUS_INVALID_DATA;
  }
  c->video_uri = tracks.video_uri; c->audio_uri = tracks.audio_uri;
  c->h264_payload_type = tracks.h264_payload_type;
  c->aac_payload_type = tracks.aac_payload_type; c->aac = tracks.aac;
  c->discovered_h264 = tracks.h264;
  aula_aac_decoder_destroy(c->aac_decoder); c->aac_decoder = NULL;
  status = c->discovery_only != 0 ? AULA_STATUS_OK :
      aula_aac_decoder_create(&c->aac, &c->aac_decoder);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtsp_native_transport_header(c, &c->video_udp_fd,
      &c->video_rtcp_fd, 0U, headers, sizeof(headers));
  return status == AULA_STATUS_OK ? aula_rtsp_native_send(c, "SETUP",
      c->video_uri.text, headers, NATIVE_SETUP_VIDEO) : status;
}

static aula_status native_setup_video(native_context *c,
                                       const aula_rtsp_message *m) {
  char headers[256]; char transport[160]; int written; aula_status status;
  if (m->session_id[0] == '\0') return AULA_STATUS_INVALID_DATA;
  (void)snprintf(c->session, sizeof(c->session), "%s", m->session_id);
  status = aula_rtsp_native_transport_header(c, &c->audio_udp_fd,
      &c->audio_rtcp_fd, 2U, transport, sizeof(transport));
  if (status != AULA_STATUS_OK) return status;
  written = snprintf(headers, sizeof(headers), "Session: %s\r\n%s", c->session,
                     transport);
  if (written < 0 || (size_t)written >= sizeof(headers)) return AULA_STATUS_LIMIT_EXCEEDED;
  return aula_rtsp_native_send(c, "SETUP", c->audio_uri.text, headers,
                                NATIVE_SETUP_AUDIO);
}

static aula_status native_keepalive_response(
    native_context *c, const aula_rtsp_message *m) {
  uint32_t session_timeout = 0U;
  uint64_t now_ns = 0U;
  if (c->keepalive_pending == 0 || m->cseq != c->keepalive_cseq ||
      m->status_code < 200U || m->status_code > 299U ||
      m->session_id[0] == '\0' || strcmp(m->session_id, c->session) != 0) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (aula_rtsp_stream_parser_session_timeout(
          c->parser, &session_timeout) != 0) {
    c->session_timeout_seconds = session_timeout;
  }
  c->keepalive_pending = 0;
  c->keepalive_cseq = 0U;
  c->keepalive_deadline_ns = 0U;
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) {
    return AULA_STATUS_IO_ERROR;
  }
  aula_rtsp_native_arm_keepalive(c, now_ns);
  return AULA_STATUS_OK;
}

static aula_status native_handshake_response(
    native_context *c, const aula_rtsp_message *m) {
  char headers[256];
  uint32_t session_timeout = 0U;
  uint64_t now_ns = 0U;
  int written;
  if (m->cseq != (uint32_t)(c->cseq - 1U) || m->status_code < 200U ||
      m->status_code > 299U) return AULA_STATUS_INVALID_DATA;
  if (m->session_id[0] != '\0' &&
      aula_rtsp_stream_parser_session_timeout(
          c->parser, &session_timeout) != 0) {
    c->session_timeout_seconds = session_timeout;
  }
  if (c->state == NATIVE_OPTIONS) return aula_rtsp_native_send(c, "DESCRIBE",
      c->video_uri.text, "Accept: application/sdp\r\n", NATIVE_DESCRIBE);
  if (c->state == NATIVE_DESCRIBE) return native_describe(c, m->body);
  if (c->state == NATIVE_SETUP_VIDEO) return native_setup_video(c, m);
  if (c->state == NATIVE_SETUP_AUDIO) {
    written = snprintf(headers, sizeof(headers), "Session: %s\r\n", c->session);
    return written < 0 || (size_t)written >= sizeof(headers) ?
        AULA_STATUS_LIMIT_EXCEEDED : aula_rtsp_native_send(c, "PLAY",
            c->aggregate_uri.text, headers, NATIVE_PLAY);
  }
  if (c->state == NATIVE_PLAY) {
    c->state = NATIVE_STREAMING; c->reconnect_attempts = 0U;
    c->reconnect_after_ns = 0U; c->health.last_error = AULA_STATUS_OK;
    if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) {
      return AULA_STATUS_IO_ERROR;
    }
    aula_rtsp_native_arm_keepalive(c, now_ns);
    return AULA_STATUS_OK;
  }
  return AULA_STATUS_STATE_ERROR;
}

static aula_status native_response(native_context *c,
                                    const aula_rtsp_message *m) {
  if (c == NULL || m == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  return c->state == NATIVE_STREAMING ? native_keepalive_response(c, m) :
      native_handshake_response(c, m);
}

static aula_status native_message(native_context *c, const aula_rtsp_message *m) {
  if (m->kind == AULA_RTSP_MESSAGE_RESPONSE) {
    int keepalive = c->state == NATIVE_STREAMING && c->keepalive_pending != 0;
    aula_status status = native_response(c, m);
    return keepalive != 0 && status != AULA_STATUS_OK &&
        status != AULA_STATUS_AGAIN ?
        aula_rtsp_native_schedule_reconnect(c, status) : status;
  }
  if (c->state != NATIVE_STREAMING) return AULA_STATUS_STATE_ERROR;
  if (m->channel == 0U) return aula_rtsp_native_push_video(c, m->body);
  if (m->channel == 2U) return aula_rtsp_native_push_audio(c, m->body);
  if (m->channel == 1U || m->channel == 3U) return
      m->body.length <= AULA_SIPD_MAX_RTCP_PACKET_BYTES ? AULA_STATUS_AGAIN :
      AULA_STATUS_LIMIT_EXCEEDED;
  return AULA_STATUS_AGAIN;
}

static aula_status native_drain_messages(native_context *c, aula_bytes input) {
  aula_rtsp_message m;
  aula_status status = aula_rtsp_stream_parser_push(c->parser, input, &m);
  while (status == AULA_STATUS_OK) {
    status = native_message(c, &m);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
    if (c->video_ready != 0 || c->audio_ready != 0) return AULA_STATUS_OK;
    status = aula_rtsp_stream_parser_push(c->parser, (aula_bytes){NULL, 0U}, &m);
  }
  return status;
}

static aula_status native_read_stream(native_context *c) {
  uint8_t input[AULA_RTSP_STREAM_BUFFER_BYTES];
  aula_status status;
  ssize_t received;
  status = native_drain_messages(c, (aula_bytes){NULL, 0U});
  if (status == AULA_STATUS_OK) return status;
  if (status != AULA_STATUS_AGAIN) return status;
  received = read(c->fd, input, sizeof(input));
  if (received < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ?
      AULA_STATUS_AGAIN : aula_rtsp_native_schedule_reconnect(c, AULA_STATUS_IO_ERROR);
  if (received == 0) return aula_rtsp_native_schedule_reconnect(c, AULA_STATUS_END);
  return native_drain_messages(c, (aula_bytes){input, (size_t)received});
}

static aula_status native_prepare_pump(native_context *c,
                                        int *out_control_checked) {
  aula_status status;
  *out_control_checked = 0;
  if (c == NULL || c->started == 0) return AULA_STATUS_STATE_ERROR;
  status = aula_rtsp_native_prepare_io(c);
  if (status != AULA_STATUS_OK) return status == AULA_STATUS_AGAIN ? status :
      aula_rtsp_native_schedule_reconnect(c, status);
  if (c->state == NATIVE_STREAMING && c->keepalive_pending != 0) {
    *out_control_checked = 1;
    status = native_read_stream(c);
    if (c->state != NATIVE_STREAMING) return status;
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  }
  return AULA_STATUS_OK;
}

static aula_status native_pump(native_context *c) {
  aula_status status;
  int control_checked = 0;
  status = native_prepare_pump(c, &control_checked);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtsp_native_expire_reorder(c);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  if (c->video_ready != 0 || c->audio_ready != 0) return AULA_STATUS_OK;
  status = aula_rtsp_native_drain_udp(c);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return
      status == AULA_STATUS_IO_ERROR ? aula_rtsp_native_schedule_reconnect(c, status) : status;
  if (c->video_ready != 0 || c->audio_ready != 0) return AULA_STATUS_OK;
  return control_checked != 0 || c->pending_length != 0U ?
      AULA_STATUS_AGAIN : native_read_stream(c);
}

static int native_discovery_timeout_ms(aula_deadline deadline) {
  uint64_t now_ns;
  uint64_t remaining_ns;
  uint64_t milliseconds;
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) return -1;
  if (now_ns >= deadline.monotonic_ns) return 0;
  remaining_ns = deadline.monotonic_ns - now_ns;
  milliseconds = (remaining_ns + UINT64_C(999999)) / UINT64_C(1000000);
  return milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
}

static nfds_t native_discovery_descriptors(
    const native_context *context, struct pollfd descriptors[5]) {
  const int media_descriptors[] = {
      context->video_udp_fd, context->video_rtcp_fd,
      context->audio_udp_fd, context->audio_rtcp_fd};
  nfds_t count = 1U;
  nfds_t index;
  descriptors[0].fd = context->fd;
  descriptors[0].events = context->state == NATIVE_CONNECTING ||
                          context->pending_length != 0U ? POLLOUT : POLLIN;
  descriptors[0].revents = 0;
  if (context->use_udp == 0 || context->state != NATIVE_STREAMING) return count;
  for (index = 0U; index < 4U; ++index) {
    if (media_descriptors[index] < 0) continue;
    descriptors[count].fd = media_descriptors[index];
    descriptors[count].events = POLLIN;
    descriptors[count].revents = 0;
    ++count;
  }
  return count;
}

static int native_discovery_descriptor_failed(
    const struct pollfd *descriptor, int control) {
  return (descriptor->revents & (POLLERR | POLLNVAL)) != 0 ||
      (control != 0 && (descriptor->revents & POLLHUP) != 0 &&
       (descriptor->revents & descriptor->events) == 0);
}

static aula_status native_discovery_poll(native_context *context,
                                          aula_deadline deadline) {
  struct pollfd descriptors[5];
  nfds_t descriptor_count;
  nfds_t index;
  int timeout_ms = native_discovery_timeout_ms(deadline);
  int result;
  if (timeout_ms < 0) return AULA_STATUS_IO_ERROR;
  if (timeout_ms == 0) return AULA_STATUS_TIMEOUT;
  descriptor_count = native_discovery_descriptors(context, descriptors);
  result = poll(descriptors, descriptor_count, timeout_ms);
  if (result < 0) return errno == EINTR ? AULA_STATUS_AGAIN : AULA_STATUS_IO_ERROR;
  if (result == 0) return AULA_STATUS_TIMEOUT;
  for (index = 0U; index < descriptor_count; ++index) {
    if (native_discovery_descriptor_failed(&descriptors[index], index == 0U))
      return AULA_STATUS_IO_ERROR;
  }
  return AULA_STATUS_OK;
}

static aula_status native_discovery_wait(native_context *context,
                                          aula_deadline deadline) {
  while (context->discovery_complete == 0) {
    aula_status status;
    status = native_discovery_poll(context, deadline);
    if (status == AULA_STATUS_AGAIN) continue;
    if (status != AULA_STATUS_OK) return status;
    status = native_pump(context);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
    if (context->discovery_complete != 0) return AULA_STATUS_OK;
  }
  return AULA_STATUS_OK;
}

static int native_config_valid(const aula_backend_config *config) {
  return config != NULL && config->name != NULL &&
      (strcmp(config->name, "rtsp_native") == 0 ||
       strcmp(config->name, "rtsp_gst_process") == 0) &&
      config->maximum_access_unit_bytes != 0U &&
      config->maximum_access_unit_bytes <= AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES &&
      config->maximum_pcm_frame_bytes >= 320U &&
      config->maximum_pcm_frame_bytes <= AULA_SIPD_MAX_AUDIO_FRAME_BYTES &&
      config->rtsp_jitter_max_ms <= AULA_RTSP_NATIVE_MAX_JITTER_MS;
}

static aula_status native_open(aula_media_backend *backend,
                                const aula_backend_config *config) {
  native_context *c = backend == NULL ? NULL : backend->context;
  aula_status status;
  if (c == NULL || c->opened != 0 || !native_config_valid(config)) return AULA_STATUS_CONFIGURATION_ERROR;
  status = aula_rtsp_parse_authorized_uri(config->video_source, config->authorized_rtsp_ipv4,
                                            &c->aggregate_uri);
  if (status != AULA_STATUS_OK) return status;
  c->video_uri = c->aggregate_uri;
  c->assembly = malloc(config->maximum_access_unit_bytes);
  c->video = malloc(config->maximum_access_unit_bytes);
  status = aula_rtsp_stream_parser_create(&c->parser);
  if (status == AULA_STATUS_OK) status = aula_h264_depacketizer_create(
      config->maximum_access_unit_bytes, &c->depacketizer);
  if (c->assembly == NULL || c->video == NULL || status != AULA_STATUS_OK) {
    aula_h264_depacketizer_destroy(c->depacketizer); aula_rtsp_stream_parser_destroy(c->parser);
    free(c->assembly); free(c->video); c->assembly = NULL; c->video = NULL;
    c->depacketizer = NULL; c->parser = NULL; return AULA_STATUS_INTERNAL_ERROR;
  }
  c->maximum_access_unit_bytes = config->maximum_access_unit_bytes;
  c->maximum_pcm_frame_bytes = config->maximum_pcm_frame_bytes;
  c->jitter_max_ms = config->rtsp_jitter_max_ms == 0U ? AULA_RTSP_NATIVE_DEFAULT_JITTER_MS : config->rtsp_jitter_max_ms;
  c->reconnect_limit = config->rtsp_reconnect_limit == 0U ? AULA_RTSP_NATIVE_DEFAULT_RECONNECTS : config->rtsp_reconnect_limit;
  c->use_udp = config->rtsp_use_udp != 0; c->fd = -1; c->video_udp_fd = -1;
  c->video_rtcp_fd = -1; c->audio_udp_fd = -1; c->audio_rtcp_fd = -1;
  c->cseq = 1U; c->opened = 1; return AULA_STATUS_OK;
}

static aula_status native_start(aula_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  aula_status status;
  if (c == NULL || c->opened == 0 || c->started != 0) return AULA_STATUS_STATE_ERROR;
  if (c->discovery_only == 0 && aula_aac_native_pipeline_available() == 0)
    return AULA_STATUS_UNSUPPORTED;
  status = aula_rtsp_native_connect(c);
  if (status != AULA_STATUS_OK) { c->health.last_error = status; return status; }
  c->started = 1; return AULA_STATUS_OK;
}

static aula_status native_read_video(aula_media_backend *backend, aula_deadline deadline,
                                      aula_media_frame *out) {
  native_context *c = backend == NULL ? NULL : backend->context; aula_status status;
  (void)deadline;
  if (c == NULL || out == NULL || c->started == 0) return AULA_STATUS_STATE_ERROR;
  if (c->video_ready == 0) {
    status = native_pump(c);
    if (status != AULA_STATUS_OK) return status;
    if (c->video_ready == 0) return AULA_STATUS_AGAIN;
  }
  (void)memset(out, 0, sizeof(*out)); out->kind = AULA_MEDIA_VIDEO_H264_ANNEX_B;
  out->data = (aula_bytes){c->video, c->video_length}; out->pts_ns = c->video_pts_ns;
  out->keyframe = c->video_keyframe; c->video_ready = 0; c->health.frames_read++;
  c->health.bytes_read += c->video_length; return AULA_STATUS_OK;
}

static aula_status native_read_audio(aula_media_backend *backend, aula_deadline deadline,
                                      aula_media_frame *out) {
  native_context *c = backend == NULL ? NULL : backend->context; aula_status status;
  (void)deadline;
  if (c == NULL || out == NULL || c->started == 0) return AULA_STATUS_STATE_ERROR;
  if (c->audio_ready == 0) {
    status = native_pump(c);
    if (status != AULA_STATUS_OK) return status;
    if (c->audio_ready == 0) return AULA_STATUS_AGAIN;
  }
  (void)memset(out, 0, sizeof(*out)); out->kind = AULA_MEDIA_AUDIO_PCM_S16LE;
  out->data = (aula_bytes){c->pcm, c->pcm_length}; out->pts_ns = c->audio_pts_ns;
  out->sample_rate = 8000U; out->channels = 1U; c->audio_ready = 0;
  c->health.frames_read++; c->health.bytes_read += c->pcm_length; return AULA_STATUS_OK;
}

static aula_status native_keyframe(aula_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  if (c == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  /* No reviewed encoder refresh command exists. Preserve intact source
   * assembly and let its periodic IDR reach the peer; resetting here can
   * repeatedly discard that IDR while feedback continues arriving. */
  return AULA_STATUS_UNSUPPORTED;
}

static aula_status native_health(const aula_media_backend *backend, aula_media_health *out) {
  const native_context *c = backend == NULL ? NULL : backend->context;
  if (c == NULL || out == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out = c->health; return AULA_STATUS_OK;
}

static aula_status native_stop(aula_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  if (c == NULL || c->opened == 0) return AULA_STATUS_STATE_ERROR;
  if (c->fd >= 0 && c->state == NATIVE_STREAMING && c->session[0] != '\0') {
    char request[320]; int written = snprintf(request, sizeof(request),
        "TEARDOWN %s RTSP/1.0\r\nCSeq: %u\r\nSession: %s\r\n\r\n", c->aggregate_uri.text,
        (unsigned int)c->cseq, c->session);
    if (written > 0 && (size_t)written < sizeof(request))
      (void)send(c->fd, request, (size_t)written, MSG_NOSIGNAL);
  }
  aula_rtsp_native_close_transports(c); c->started = 0; c->state = NATIVE_CLOSED;
  c->pending_length = 0U; c->pending_offset = 0U; c->video_ready = 0; c->audio_ready = 0;
  c->assembly_length = 0U; c->video_length = 0U; c->pcm_length = 0U; c->video_keyframe = 0;
  c->video_synchronized = 0;
  c->session_timeout_seconds = 0U; c->keepalive_cseq = 0U;
  c->keepalive_due_ns = 0U; c->keepalive_deadline_ns = 0U;
  c->keepalive_pending = 0;
  aula_rtsp_native_clear_reorder(&c->video_reorder); aula_rtsp_native_clear_reorder(&c->audio_reorder);
  aula_aac_decoder_destroy(c->aac_decoder); c->aac_decoder = NULL; return AULA_STATUS_OK;
}

static void native_close(aula_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  if (c == NULL) return;
  if (c->started != 0) (void)native_stop(backend);
  aula_h264_depacketizer_destroy(c->depacketizer); aula_rtsp_stream_parser_destroy(c->parser);
  aula_aac_decoder_destroy(c->aac_decoder); free(c->assembly); free(c->video); free(c);
  backend->context = NULL; backend->vtable = NULL;
}

static const aula_media_backend_vtable NATIVE_VTABLE = {native_open, native_start,
    native_read_video, native_read_audio, native_keyframe, native_health, native_stop, native_close};

aula_status aula_rtsp_native_backend_create(aula_media_backend *out) {
  native_context *c;
  if (out == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  c = calloc(1U, sizeof(*c)); if (c == NULL) return AULA_STATUS_INTERNAL_ERROR;
  c->fd = -1; c->video_udp_fd = -1; c->video_rtcp_fd = -1; c->audio_udp_fd = -1; c->audio_rtcp_fd = -1;
  out->vtable = &NATIVE_VTABLE; out->context = c; return AULA_STATUS_OK;
}

aula_status aula_rtsp_native_discover_h264(
    const aula_backend_config *config, aula_deadline deadline,
    aula_rtsp_h264_capability *out_capability) {
  aula_media_backend backend = {0};
  native_context *context;
  aula_status status;
  if (config == NULL || out_capability == NULL || deadline.monotonic_ns == 0U)
    return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_capability, 0, sizeof(*out_capability));
  status = aula_rtsp_native_backend_create(&backend);
  if (status == AULA_STATUS_OK) status = backend.vtable->open(&backend, config);
  context = (native_context *)backend.context;
  if (status == AULA_STATUS_OK) {
    context->reconnect_limit = 0U;
    context->discovery_only = 1;
    status = native_start(&backend);
  }
  if (status == AULA_STATUS_OK) status = native_discovery_wait(context, deadline);
  if (status == AULA_STATUS_OK) *out_capability = context->discovered_h264;
  if (backend.vtable != NULL) backend.vtable->close(&backend);
  return status;
}

aula_status aula_rtsp_native_backend_expect_h264(
    aula_media_backend *backend,
    const aula_rtsp_h264_capability *capability) {
  native_context *context = backend == NULL ? NULL : backend->context;
  if (backend == NULL || backend->vtable != &NATIVE_VTABLE || context == NULL ||
      capability == NULL || context->opened != 0 ||
      !aula_h264_profile_level_id_valid(capability->profile_level_id)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  context->expected_h264 = *capability;
  context->expected_h264_present = 1;
  return AULA_STATUS_OK;
}

aula_status aula_rtsp_gst_process_backend_create(aula_media_backend *out) {
  return aula_rtsp_native_backend_create(out);
}
