#include "rtsp_private.h"
#include "aula_sipd/media_aac.h"

#include <stdio.h>
#include <string.h>

static uint64_t native_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint32_t native_keepalive_interval_seconds(
    const native_context *context) {
  uint32_t timeout = context->session_timeout_seconds == 0U ?
      AULA_RTSP_NATIVE_DEFAULT_SESSION_TIMEOUT_SECONDS :
      context->session_timeout_seconds;
  uint32_t interval = timeout / 3U;
  if (interval == 0U) interval = 1U;
  if (interval > AULA_RTSP_NATIVE_MAX_KEEPALIVE_INTERVAL_SECONDS) {
    interval = AULA_RTSP_NATIVE_MAX_KEEPALIVE_INTERVAL_SECONDS;
  }
  return interval;
}

void aula_rtsp_native_arm_keepalive(native_context *context, uint64_t now_ns) {
  uint64_t interval_ns;
  if (context == NULL) return;
  interval_ns = (uint64_t)native_keepalive_interval_seconds(context) *
      UINT64_C(1000000000);
  context->keepalive_due_ns = native_saturating_add(now_ns, interval_ns);
}

static aula_status native_prepare_keepalive(native_context *context,
                                             uint64_t now_ns) {
  char headers[192];
  uint32_t response_seconds;
  uint32_t interval_seconds;
  uint32_t expected_cseq;
  int written;
  aula_status status;
  if (context->state != NATIVE_STREAMING || context->session[0] == '\0') {
    return AULA_STATUS_OK;
  }
  if (context->keepalive_pending != 0) {
    return context->keepalive_deadline_ns != 0U &&
                   now_ns >= context->keepalive_deadline_ns ?
        AULA_STATUS_TIMEOUT : AULA_STATUS_OK;
  }
  if (context->keepalive_due_ns == 0U) {
    aula_rtsp_native_arm_keepalive(context, now_ns);
    return AULA_STATUS_OK;
  }
  if (now_ns < context->keepalive_due_ns) return AULA_STATUS_OK;
  written = snprintf(headers, sizeof(headers),
                     "Session: %s\r\nContent-Length: 0\r\n",
                     context->session);
  if (written < 0 || (size_t)written >= sizeof(headers)) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  expected_cseq = context->cseq;
  status = aula_rtsp_native_send(context, "GET_PARAMETER",
                                  context->aggregate_uri.text, headers,
                                  NATIVE_STREAMING);
  if (status != AULA_STATUS_OK) return status;
  interval_seconds = native_keepalive_interval_seconds(context);
  response_seconds = interval_seconds <
      AULA_RTSP_NATIVE_KEEPALIVE_RESPONSE_SECONDS ? interval_seconds :
      AULA_RTSP_NATIVE_KEEPALIVE_RESPONSE_SECONDS;
  context->keepalive_cseq = expected_cseq;
  context->keepalive_pending = 1;
  context->keepalive_deadline_ns = native_saturating_add(
      now_ns, (uint64_t)response_seconds * UINT64_C(1000000000));
  return AULA_STATUS_OK;
}

aula_status aula_rtsp_native_reset_stream(native_context *context) {
  aula_status status;
  if (context == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  aula_rtsp_stream_parser_destroy(context->parser);
  context->parser = NULL;
  status = aula_rtsp_stream_parser_create(&context->parser);
  if (status != AULA_STATUS_OK) return status;
  aula_h264_depacketizer_destroy(context->depacketizer);
  context->depacketizer = NULL;
  status = aula_h264_depacketizer_create(context->maximum_access_unit_bytes,
                                           &context->depacketizer);
  if (status != AULA_STATUS_OK) return status;
  aula_aac_decoder_destroy(context->aac_decoder);
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
  aula_rtsp_native_clear_reorder(&context->video_reorder);
  aula_rtsp_native_clear_reorder(&context->audio_reorder);
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
  return AULA_STATUS_OK;
}

aula_status aula_rtsp_native_schedule_reconnect(native_context *context,
                                                   aula_status cause) {
  uint64_t now_ns = 0U;
  uint32_t delay_ms;
  unsigned int shift;
  aula_status status;
  if (context == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  aula_rtsp_native_close_transports(context);
  status = aula_rtsp_native_reset_stream(context);
  if (status != AULA_STATUS_OK) {
    context->health.last_error = status;
    return status;
  }
  if (context->reconnect_attempts >= context->reconnect_limit) {
    context->health.last_error = cause;
    return cause;
  }
  shift = context->reconnect_attempts > 3U ? 3U : context->reconnect_attempts;
  delay_ms = AULA_RTSP_NATIVE_RECONNECT_BASE_MS << shift;
  if (delay_ms > AULA_RTSP_NATIVE_RECONNECT_MAX_MS) {
    delay_ms = AULA_RTSP_NATIVE_RECONNECT_MAX_MS;
  }
  context->reconnect_attempts++;
  (void)aula_platform_monotonic_now(&now_ns);
  context->reconnect_after_ns = native_saturating_add(
      now_ns, (uint64_t)delay_ms * UINT64_C(1000000));
  context->health.restarts++;
  context->health.last_error = cause;
  return AULA_STATUS_AGAIN;
}

aula_status aula_rtsp_native_prepare_io(native_context *context) {
  aula_status status;
  uint64_t now_ns = 0U;
  if (context == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (context->state == NATIVE_CLOSED && context->started != 0) {
    (void)aula_platform_monotonic_now(&now_ns);
    if (now_ns < context->reconnect_after_ns) return AULA_STATUS_AGAIN;
    status = aula_rtsp_native_connect(context);
    if (status != AULA_STATUS_OK) {
      return aula_rtsp_native_schedule_reconnect(context, status);
    }
  }
  if (context->state == NATIVE_CONNECTING) {
    status = aula_rtsp_native_start_request(context);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  }
  if (context->state == NATIVE_STREAMING) {
    status = aula_platform_monotonic_now(&now_ns);
    if (status != AULA_STATUS_OK) return status;
    status = native_prepare_keepalive(context, now_ns);
    if (status != AULA_STATUS_OK) return status;
  }
  status = aula_rtsp_native_flush(context);
  if (status == AULA_STATUS_AGAIN && context->state == NATIVE_STREAMING &&
      context->keepalive_pending != 0) return AULA_STATUS_OK;
  return status;
}
