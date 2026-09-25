#include "rtsp_private.h"
#include "ls200_sipd/media_aac.h"

#include <stdio.h>
#include <string.h>

static uint64_t native_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint32_t native_keepalive_interval_seconds(
    const native_context *context) {
  uint32_t timeout = context->session_timeout_seconds == 0U ?
      LS200_RTSP_NATIVE_DEFAULT_SESSION_TIMEOUT_SECONDS :
      context->session_timeout_seconds;
  uint32_t interval = timeout / 3U;
  if (interval == 0U) interval = 1U;
  if (interval > LS200_RTSP_NATIVE_MAX_KEEPALIVE_INTERVAL_SECONDS) {
    interval = LS200_RTSP_NATIVE_MAX_KEEPALIVE_INTERVAL_SECONDS;
  }
  return interval;
}

void ls200_rtsp_native_arm_keepalive(native_context *context, uint64_t now_ns) {
  uint64_t interval_ns;
  if (context == NULL) return;
  interval_ns = (uint64_t)native_keepalive_interval_seconds(context) *
      UINT64_C(1000000000);
  context->keepalive_due_ns = native_saturating_add(now_ns, interval_ns);
}

static ls200_status native_prepare_keepalive(native_context *context,
                                             uint64_t now_ns) {
  char headers[192];
  uint32_t response_seconds;
  uint32_t interval_seconds;
  uint32_t expected_cseq;
  int written;
  ls200_status status;
  if (context->state != NATIVE_STREAMING || context->session[0] == '\0') {
    return LS200_STATUS_OK;
  }
  if (context->keepalive_pending != 0) {
    return context->keepalive_deadline_ns != 0U &&
                   now_ns >= context->keepalive_deadline_ns ?
        LS200_STATUS_TIMEOUT : LS200_STATUS_OK;
  }
  if (context->keepalive_due_ns == 0U) {
    ls200_rtsp_native_arm_keepalive(context, now_ns);
    return LS200_STATUS_OK;
  }
  if (now_ns < context->keepalive_due_ns) return LS200_STATUS_OK;
  written = snprintf(headers, sizeof(headers),
                     "Session: %s\r\nContent-Length: 0\r\n",
                     context->session);
  if (written < 0 || (size_t)written >= sizeof(headers)) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  expected_cseq = context->cseq;
  status = ls200_rtsp_native_send(context, "GET_PARAMETER",
                                  context->aggregate_uri.text, headers,
                                  NATIVE_STREAMING);
  if (status != LS200_STATUS_OK) return status;
  interval_seconds = native_keepalive_interval_seconds(context);
  response_seconds = interval_seconds <
      LS200_RTSP_NATIVE_KEEPALIVE_RESPONSE_SECONDS ? interval_seconds :
      LS200_RTSP_NATIVE_KEEPALIVE_RESPONSE_SECONDS;
  context->keepalive_cseq = expected_cseq;
  context->keepalive_pending = 1;
  context->keepalive_deadline_ns = native_saturating_add(
      now_ns, (uint64_t)response_seconds * UINT64_C(1000000000));
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_reset_stream(native_context *context) {
  ls200_status status;
  if (context == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  ls200_rtsp_stream_parser_destroy(context->parser);
  context->parser = NULL;
  status = ls200_rtsp_stream_parser_create(&context->parser);
  if (status != LS200_STATUS_OK) return status;
  ls200_h264_depacketizer_destroy(context->depacketizer);
  context->depacketizer = NULL;
  status = ls200_h264_depacketizer_create(context->maximum_access_unit_bytes,
                                           &context->depacketizer);
  if (status != LS200_STATUS_OK) return status;
  ls200_aac_decoder_destroy(context->aac_decoder);
  context->aac_decoder = NULL;
  (void)memset(&context->parameter_sets, 0, sizeof(context->parameter_sets));
  (void)memset(&context->aac, 0, sizeof(context->aac));
  context->assembly_length = 0U;
  context->video_length = 0U;
  context->pcm_length = 0U;
  context->video_ready = 0;
  context->audio_ready = 0;
  context->video_keyframe = 0;
  context->video_synchronized = 0;
  context->video_pts_ns = 0U;
  context->audio_pts_ns = 0U;
  context->video_source_port = 0U;
  context->video_rtcp_source_port = 0U;
  context->audio_source_port = 0U;
  context->audio_rtcp_source_port = 0U;
  ls200_rtsp_native_clear_reorder(&context->video_reorder);
  ls200_rtsp_native_clear_reorder(&context->audio_reorder);
  (void)memset(context->session, 0, sizeof(context->session));
  context->pending_length = 0U;
  context->pending_offset = 0U;
  context->session_timeout_seconds = 0U;
  context->keepalive_cseq = 0U;
  context->keepalive_due_ns = 0U;
  context->keepalive_deadline_ns = 0U;
  context->keepalive_pending = 0;
  context->cseq = 1U;
  context->state = NATIVE_CLOSED;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_schedule_reconnect(native_context *context,
                                                   ls200_status cause) {
  uint64_t now_ns = 0U;
  uint32_t delay_ms;
  unsigned int shift;
  ls200_status status;
  if (context == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  ls200_rtsp_native_close_transports(context);
  status = ls200_rtsp_native_reset_stream(context);
  if (status != LS200_STATUS_OK) {
    context->health.last_error = status;
    return status;
  }
  if (context->reconnect_attempts >= context->reconnect_limit) {
    context->health.last_error = cause;
    return cause;
  }
  shift = context->reconnect_attempts > 3U ? 3U : context->reconnect_attempts;
  delay_ms = LS200_RTSP_NATIVE_RECONNECT_BASE_MS << shift;
  if (delay_ms > LS200_RTSP_NATIVE_RECONNECT_MAX_MS) {
    delay_ms = LS200_RTSP_NATIVE_RECONNECT_MAX_MS;
  }
  context->reconnect_attempts++;
  (void)ls200_platform_monotonic_now(&now_ns);
  context->reconnect_after_ns = native_saturating_add(
      now_ns, (uint64_t)delay_ms * UINT64_C(1000000));
  context->health.restarts++;
  context->health.last_error = cause;
  return LS200_STATUS_AGAIN;
}

ls200_status ls200_rtsp_native_prepare_io(native_context *context) {
  ls200_status status;
  uint64_t now_ns = 0U;
  if (context == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (context->state == NATIVE_CLOSED && context->started != 0) {
    (void)ls200_platform_monotonic_now(&now_ns);
    if (now_ns < context->reconnect_after_ns) return LS200_STATUS_AGAIN;
    status = ls200_rtsp_native_connect(context);
    if (status != LS200_STATUS_OK) {
      return ls200_rtsp_native_schedule_reconnect(context, status);
    }
  }
  if (context->state == NATIVE_CONNECTING) {
    status = ls200_rtsp_native_start_request(context);
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  }
  if (context->state == NATIVE_STREAMING) {
    status = ls200_platform_monotonic_now(&now_ns);
    if (status != LS200_STATUS_OK) return status;
    status = native_prepare_keepalive(context, now_ns);
    if (status != LS200_STATUS_OK) return status;
  }
  status = ls200_rtsp_native_flush(context);
  if (status == LS200_STATUS_AGAIN && context->state == NATIVE_STREAMING &&
      context->keepalive_pending != 0) return LS200_STATUS_OK;
  return status;
}
