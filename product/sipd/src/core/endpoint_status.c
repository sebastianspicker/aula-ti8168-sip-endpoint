#include "endpoint_internal.h"

#include <stdio.h>
#include <string.h>

const char *endpoint_call_state_name(aula_call_state state) {
  static const char *const names[] = {
    "idle", "resolving", "inviting", "early", "establishing_media",
    "established", "terminating", "backing_off", "failed", "terminated",
    "stopped", "terminal_failure"
  };
  return state >= AULA_CALL_IDLE && state <= AULA_CALL_TERMINAL_FAILURE
      ? names[state] : "invalid";
}

void endpoint_log(aula_endpoint *endpoint, aula_log_level level,
                  const char *event, const char *reason) {
  aula_log_event record;
  if (endpoint == NULL || endpoint->logger == NULL) return;
  (void)memset(&record, 0, sizeof(record));
  record.level = level;
  record.component = "endpoint";
  record.event = event;
  record.correlation_id = aula_call_get_correlation_id(endpoint->call);
  record.call_state = endpoint_call_state_name(aula_call_get_state(endpoint->call));
  record.reason_code = reason;
  record.rx_rendering = endpoint->status.rx_rendering;
  (void)aula_log_write(endpoint->logger, &record);
}

void endpoint_report_media_totals(aula_media_session *session) {
  aula_media_session_status status;
  if (session == NULL || aula_media_session_get_status(session, &status) !=
      AULA_STATUS_OK || status.state != AULA_MEDIA_SESSION_COMMITTED) return;
  /* Fixed labels and counters only: no media, peer identity or key material. */
  (void)fprintf(stderr,
      "aula-sipd: media-summary video_sent=%llu audio_sent=%llu "
      "accepted=%llu rejected=%llu keyframes=%llu\n",
      (unsigned long long)status.video_packets_sent,
      (unsigned long long)status.audio_packets_sent,
      (unsigned long long)status.receive.accepted_packets,
      (unsigned long long)status.receive.rejected_packets,
      (unsigned long long)status.keyframe_requests);
}

void endpoint_refresh_status(aula_endpoint *endpoint) {
  aula_call_backoff backoff;
  if (endpoint == NULL) return;
  (void)memset(&endpoint->status, 0, sizeof(endpoint->status));
  endpoint->status.call_state = aula_call_get_state(endpoint->call);
  if (endpoint->event_state_initialized == 0 ||
      endpoint->event_call_state != endpoint->status.call_state) {
    if (endpoint->event_revision != UINT64_MAX) ++endpoint->event_revision;
    endpoint->event_call_state = endpoint->status.call_state;
    endpoint->event_state_initialized = 1;
  }
  endpoint->status.sip_profile = endpoint->sip_config.profile;
  endpoint->status.sip_transport = endpoint->sip_config.preferred_transport;
  if (aula_sip_adapter_get_registration_status(
          endpoint->adapter, &endpoint->status.registration) != AULA_STATUS_OK) {
    endpoint->status.registration.state =
        endpoint->sip_config.profile == AULA_ZOOM_PROFILE_PROXY_REGISTRATION
            ? AULA_SIP_REGISTRATION_FAILED
            : AULA_SIP_REGISTRATION_DISABLED;
  }
  endpoint->status.rx_rendering = 0;
  if (endpoint->media != NULL) {
    aula_media_session_status media;
    if (aula_media_session_get_status(endpoint->media, &media) == AULA_STATUS_OK) {
      endpoint->status.media_session_present = 1;
      endpoint->status.rx_rendering = media.rx_rendering;
      endpoint->status.media_session_state = media.state;
      endpoint->status.video_backend = media.backend;
      endpoint->status.audio_backend = media.backend;
      endpoint->status.receive_shim = media.receive;
      endpoint->status.video_rtp = media.video_rtp;
      endpoint->status.audio_rtp = media.audio_rtp;
      endpoint->status.video_packets_queued = media.video_packets_queued;
      endpoint->status.audio_packets_queued = media.audio_packets_queued;
      endpoint->status.video_packets_sent = media.video_packets_sent;
      endpoint->status.audio_packets_sent = media.audio_packets_sent;
      endpoint->status.dropped_video_packets = media.dropped_video_packets;
      endpoint->status.dropped_audio_packets = media.dropped_audio_packets;
      endpoint->status.keyframe_requests = media.keyframe_requests;
      endpoint->status.keyframe_request_failures = media.keyframe_request_failures;
      endpoint->status.rejected_dtmf_requests = media.rejected_dtmf_requests;
      endpoint->status.inbound_dtmf = media.inbound_dtmf;
      endpoint->status.video_payload_type = media.video_payload_type;
      endpoint->status.audio_payload_type = media.audio_payload_type;
      endpoint->status.video_srtp = media.video_srtp;
      endpoint->status.audio_srtp = media.audio_srtp;
      (void)memcpy(endpoint->status.active_h264_profile_level_id,
                   media.active_h264_profile_level_id,
                   sizeof(endpoint->status.active_h264_profile_level_id));
      endpoint->status.video_direction = media.video_direction;
      endpoint->status.audio_direction = media.audio_direction;
      endpoint->status.video_transmit_enabled = media.video_transmit_enabled;
      endpoint->status.audio_muted = media.audio_muted;
      endpoint->status.aec = media.aec;
      endpoint->status.rx_rendering = media.rx_rendering;
      endpoint->status.rx_audio_rendering = media.rx_audio_rendering;
      endpoint->status.rx_video_rendering = media.rx_video_rendering;
      endpoint->status.rx_audio_render_fresh = media.rx_audio_render_fresh;
      endpoint->status.rx_video_render_fresh = media.rx_video_render_fresh;
      endpoint->status.rx_renderer_healthy = media.rx_renderer_healthy;
      endpoint->status.rx_audio_last_success_ns = media.rx_audio_last_success_ns;
      endpoint->status.rx_video_last_success_ns = media.rx_video_last_success_ns;
      endpoint->status.last_error = media.last_error;
    }
  }
  if (endpoint->last_endpoint_error != AULA_STATUS_OK)
    endpoint->status.last_error = endpoint->last_endpoint_error;
  if (aula_call_get_backoff(endpoint->call, &backoff) == AULA_STATUS_OK)
    endpoint->status.reconnect_attempts = backoff.attempt;
  if (endpoint->control != NULL)
    (void)aula_control_server_publish_status(endpoint->control, &endpoint->status);
}
