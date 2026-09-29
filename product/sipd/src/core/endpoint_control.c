#include "endpoint_internal.h"
#include "../control/request_decode.h"
#include "aula_sipd/platform.h"

#include <stdio.h>
#include <string.h>

static void endpoint_secure_zero(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) { *cursor = 0U; ++cursor; --length; }
}

aula_status aula_endpoint_send_dtmf(aula_endpoint *endpoint, uint8_t digit,
                                      uint16_t duration_samples, int end) {
  uint64_t now;
  aula_status status;
  if (endpoint == NULL || digit > 15U || duration_samples == 0U ||
      (end != 0 && end != 1)) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_call_get_state(endpoint->call) != AULA_CALL_ESTABLISHED ||
      endpoint->media == NULL) return AULA_STATUS_STATE_ERROR;
  if (endpoint->dtmf_end_packets_remaining != 0U ||
      endpoint->layout_next_digits_pending != 0U) return AULA_STATUS_AGAIN;
  if (end == 0)
    return aula_media_session_send_dtmf(endpoint->media, digit,
                                         duration_samples, 0);
  status = aula_media_session_send_dtmf(endpoint->media, digit, 160U, 0);
  if (status != AULA_STATUS_OK) return status;
  if (aula_platform_monotonic_now(&now) != AULA_STATUS_OK)
    return AULA_STATUS_IO_ERROR;
  endpoint->dtmf_digit = digit;
  endpoint->dtmf_duration_samples = duration_samples < 160U
      ? 160U : duration_samples;
  endpoint->dtmf_end_packets_remaining = 3U;
  endpoint->dtmf_next_emit_ns = endpoint_deadline_after(
      now, AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS);
  return AULA_STATUS_OK;
}

void endpoint_clear_dtmf_schedule(aula_endpoint *endpoint) {
  if (endpoint == NULL) return;
  endpoint->dtmf_end_packets_remaining = 0U;
  endpoint->dtmf_next_emit_ns = 0U;
  endpoint->layout_next_digits_pending = 0U;
}

static aula_status endpoint_start_layout_next(aula_endpoint *endpoint) {
  aula_status status;
  if (endpoint == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (endpoint->dtmf_end_packets_remaining != 0U ||
      endpoint->layout_next_digits_pending != 0U) return AULA_STATUS_AGAIN;
  status = aula_endpoint_send_dtmf(endpoint, 1U, 800U, 1);
  if (status == AULA_STATUS_OK) endpoint->layout_next_digits_pending = 1U;
  return status;
}

void endpoint_poll_dtmf(aula_endpoint *endpoint, uint64_t now) {
  aula_status status;
  if (endpoint == NULL || now < endpoint->dtmf_next_emit_ns) return;
  if (endpoint->dtmf_end_packets_remaining == 0U) {
    if (endpoint->layout_next_digits_pending == 0U) return;
    status = aula_media_session_send_dtmf(endpoint->media, 1U, 160U, 0);
    if (status == AULA_STATUS_OK) {
      endpoint->dtmf_digit = 1U;
      endpoint->dtmf_duration_samples = 800U;
      endpoint->dtmf_end_packets_remaining = 3U;
      --endpoint->layout_next_digits_pending;
      endpoint->dtmf_next_emit_ns = endpoint_deadline_after(
          now, AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS);
    } else if (status == AULA_STATUS_AGAIN) {
      endpoint->dtmf_next_emit_ns = endpoint_deadline_after(
          now, AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS);
    } else {
      endpoint_clear_dtmf_schedule(endpoint);
    }
    return;
  }
  status = aula_media_session_send_dtmf(endpoint->media, endpoint->dtmf_digit,
      endpoint->dtmf_duration_samples, 1);
  if (status == AULA_STATUS_OK) {
    --endpoint->dtmf_end_packets_remaining;
    endpoint->dtmf_next_emit_ns = endpoint_deadline_after(
        now, AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS);
  } else if (status != AULA_STATUS_AGAIN) {
    endpoint_clear_dtmf_schedule(endpoint);
  }
}

static int endpoint_state_is_inviting(aula_call_state state) {
  return state == AULA_CALL_INVITING || state == AULA_CALL_EARLY;
}

static int endpoint_state_has_media(aula_call_state state) {
  return state == AULA_CALL_ESTABLISHING_MEDIA ||
         state == AULA_CALL_ESTABLISHED;
}

static int endpoint_state_is_quiescent(aula_call_state state) {
  return state == AULA_CALL_IDLE || state == AULA_CALL_RESOLVING ||
         state == AULA_CALL_BACKING_OFF || state == AULA_CALL_FAILED ||
         state == AULA_CALL_TERMINAL_FAILURE;
}

static int endpoint_state_is_stopped(aula_call_state state) {
  return state == AULA_CALL_TERMINATED || state == AULA_CALL_STOPPED;
}

void endpoint_finish_shutdown(aula_endpoint *endpoint) {
  if (endpoint == NULL) return;
  endpoint->shutdown_requested = 1;
  endpoint->invite_deadline_ns = 0U;
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  endpoint_retire_dialog(endpoint);
  aula_sip_adapter_clear_outbound_target(endpoint->adapter);
  (void)aula_call_apply_event(endpoint->call, AULA_CALL_EVENT_SHUTDOWN, NULL);
  endpoint->shutdown_deadline_ns = 0U;
  endpoint_clear_dtmf_schedule(endpoint);
}

static aula_status endpoint_control_command(void *context,
                                             aula_control_command command) {
  return aula_endpoint_command((aula_endpoint *)context, command);
}

static aula_status endpoint_control_originate(aula_endpoint *endpoint,
                                               aula_bytes payload) {
  aula_control_originate_request decoded;
  aula_status status = aula_control_decode_originate(payload, &decoded);
  if (status == AULA_STATUS_OK)
    status = aula_endpoint_originate(endpoint, &decoded.dial);
  endpoint_secure_zero(&decoded, sizeof(decoded));
  return status;
}

static aula_status endpoint_control_dtmf(aula_endpoint *endpoint,
                                          aula_bytes payload) {
  uint8_t digit = 0U;
  aula_status status = aula_control_decode_dtmf(payload, &digit);
  return status == AULA_STATUS_OK
      ? aula_endpoint_send_dtmf(endpoint, digit, 800U, 1) : status;
}

static aula_status endpoint_control_media(aula_endpoint *endpoint,
                                           aula_bytes payload) {
  aula_control_media_request decoded;
  aula_status status = aula_control_decode_media(payload, &decoded);
  if (status != AULA_STATUS_OK) return status;
  if (endpoint == NULL || endpoint->settings_media_managed == 0 || endpoint->media == NULL ||
      aula_call_get_state(endpoint->call) != AULA_CALL_ESTABLISHED) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (decoded.action == AULA_CONTROL_MEDIA_VIDEO_TRANSMIT) {
    status = aula_media_session_set_video_transmit_enabled(
        endpoint->media, decoded.video_transmit_enabled);
  } else if (decoded.action == AULA_CONTROL_MEDIA_AUDIO_MUTE) {
    status = aula_media_session_set_audio_muted(endpoint->media,
                                                  decoded.audio_muted);
  } else if (decoded.action == AULA_CONTROL_MEDIA_KEYFRAME) {
    status = aula_media_session_request_video_keyframe(endpoint->media);
  } else if (decoded.action == AULA_CONTROL_MEDIA_LAYOUT_NEXT) {
    status = endpoint_start_layout_next(endpoint);
  } else {
    status = AULA_STATUS_INVALID_DATA;
  }
  endpoint_refresh_status(endpoint);
  return status;
}

static const char *endpoint_settings_profile_name(aula_zoom_profile profile) {
  static const char *const names[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  return profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB ? names[profile] : NULL;
}

static aula_status endpoint_write_settings(const aula_endpoint *endpoint,
                                            aula_mutable_bytes *response) {
  const char *profile;
  int length;
  if (endpoint == NULL || response == NULL || response->data == NULL ||
      endpoint->settings_revision == 0U ||
      (profile = endpoint_settings_profile_name(endpoint->sip_config.profile)) == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  length = snprintf((char *)response->data, response->capacity,
      "{\"revision\":%u,\"profile\":\"%s\",\"media\":\"%s\",\"tls\":\"%s\"}",
      endpoint->settings_revision, profile,
      endpoint->settings_media_managed != 0 ? "managed" : "disabled",
      endpoint->sip_config.enable_tls != 0 &&
              endpoint->sip_config.preferred_transport == AULA_TRANSPORT_TLS
          ? "required" : "not_required");
  if (length < 0 || (size_t)length >= response->capacity) return AULA_STATUS_INTERNAL_ERROR;
  response->length = (size_t)length;
  return AULA_STATUS_OK;
}

static aula_status endpoint_settings_restore_policy(
    aula_endpoint *endpoint, aula_zoom_profile profile, int enable_tls,
    aula_transport transport, aula_status persistence_failure) {
  aula_status rollback = aula_sip_adapter_apply_policy(
      endpoint->adapter, profile, enable_tls, transport);
  return rollback == AULA_STATUS_OK ? persistence_failure
                                     : AULA_STATUS_INTERNAL_ERROR;
}

aula_status endpoint_control_settings(aula_endpoint *endpoint,
                                       aula_bytes payload,
                                       aula_mutable_bytes *response) {
  aula_control_settings_request decoded;
  aula_zoom_profile previous_profile;
  aula_transport previous_transport;
  aula_transport candidate_transport;
  int previous_enable_tls;
  int candidate_enable_tls;
  aula_status status;
  if (endpoint == NULL || response == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_control_payload_is_empty_object(payload)) return endpoint_write_settings(endpoint, response);
  status = aula_control_decode_settings(payload, &decoded);
  if (status != AULA_STATUS_OK) return status;
  if (decoded.revision != endpoint->settings_revision) return AULA_STATUS_CONFLICT;
  if (endpoint->settings_revision == UINT32_MAX) return AULA_STATUS_CONFLICT;
  status = endpoint_settings_validate_candidate(endpoint, decoded.profile,
                                                decoded.media_managed);
  if (status != AULA_STATUS_OK) return status;
  if (endpoint->adapter == NULL) return AULA_STATUS_STATE_ERROR;
  previous_profile = endpoint->sip_config.profile;
  previous_enable_tls = endpoint->sip_config.enable_tls;
  previous_transport = endpoint->sip_config.preferred_transport;
  candidate_enable_tls = decoded.profile == AULA_ZOOM_PROFILE_PRIVATE_LAB
                             ? previous_enable_tls
                             : 1;
  candidate_transport = decoded.profile == AULA_ZOOM_PROFILE_PRIVATE_LAB
                            ? previous_transport
                            : AULA_TRANSPORT_TLS;
  /* Adapter publication is bounded and in-memory.  Apply it first so every
   * possible adapter rejection happens before the durable rename.  If any
   * pre-commit persistence step fails, restore the previous adapter policy;
   * the endpoint is single-threaded and idle throughout this transaction. */
  status = aula_sip_adapter_apply_policy(endpoint->adapter, decoded.profile,
                                           candidate_enable_tls,
                                           candidate_transport);
  if (status != AULA_STATUS_OK) return status;
  status = endpoint_settings_persist(endpoint, endpoint->settings_revision + 1U,
                                     decoded.profile, decoded.media_managed);
  if (status != AULA_STATUS_OK &&
      status != AULA_STATUS_PERSISTENCE_UNCERTAIN) {
    return endpoint_settings_restore_policy(endpoint, previous_profile,
        previous_enable_tls, previous_transport, status);
  }
  endpoint->sip_config.profile = decoded.profile;
  endpoint->sip_config.enable_tls = candidate_enable_tls;
  endpoint->sip_config.preferred_transport = candidate_transport;
  endpoint->settings_media_managed = decoded.media_managed;
  ++endpoint->settings_revision;
  if (status == AULA_STATUS_PERSISTENCE_UNCERTAIN) return status;
  return endpoint_write_settings(endpoint, response);
}

static aula_status endpoint_control_replace_credential(aula_endpoint *endpoint,
                                                         aula_bytes payload) {
  aula_control_credentials_request decoded;
  aula_status status = aula_control_decode_credentials(payload, &decoded);
  if (status == AULA_STATUS_OK &&
      (endpoint->view->auth_username == NULL ||
       strcmp(decoded.username, endpoint->view->auth_username) != 0)) {
    status = AULA_STATUS_PERMISSION_DENIED;
  }
  if (status == AULA_STATUS_OK &&
      (endpoint->view->auth_secret_file == NULL ||
       endpoint->view->auth_secret_file[0] == '\0')) {
    status = AULA_STATUS_CONFIGURATION_ERROR;
  }
  if (status == AULA_STATUS_OK) {
    aula_bytes secret = {(const uint8_t *)decoded.password, strlen(decoded.password)};
    status = aula_config_replace_secret_file(endpoint->view->auth_secret_file, secret);
  }
  endpoint_secure_zero(&decoded, sizeof(decoded));
  return status;
}

int endpoint_diagnostics_request_is_valid(aula_bytes payload) {
  return payload.data != NULL && payload.length == 2U &&
      memcmp(payload.data, "{}", 2U) == 0;
}

static const char *endpoint_diagnostics_configuration_state(
    const aula_endpoint *endpoint) {
  return endpoint->view != NULL && endpoint->view->enable_local_control != 0
      ? "ready" : "not_ready";
}

static const char *endpoint_diagnostics_sip_control_state(
    const aula_endpoint *endpoint) {
  return endpoint->status.registration.state == AULA_SIP_REGISTRATION_REGISTERED
      || endpoint->status.call_state == AULA_CALL_ESTABLISHED
      ? "ready" : "not_ready";
}

static const char *endpoint_diagnostics_media_state(
    const aula_endpoint *endpoint) {
  return endpoint->status.media_session_present != 0 ? "not_ready" : "unavailable";
}

static const char *endpoint_diagnostics_tls_profile_policy_state(
    const aula_endpoint *endpoint) {
  return endpoint->view != NULL &&
      endpoint->status.sip_profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB &&
      endpoint->status.sip_transport <= AULA_TRANSPORT_TLS &&
      (endpoint->status.sip_transport != AULA_TRANSPORT_TLS ||
       endpoint->view->enable_tls != 0)
      ? "ready" : "not_ready";
}

aula_status endpoint_write_diagnostics(aula_endpoint *endpoint,
                                        aula_mutable_bytes *response) {
  const char *configuration;
  const char *sip_control;
  const char *media;
  const char *tls_profile_policy;
  int length;
  if (endpoint == NULL || response == NULL || response->data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  /* Refreshing reads the current adapter/media snapshots only.  It does not
   * initiate network traffic, invoke a shell, or expose configuration text. */
  endpoint_refresh_status(endpoint);
  configuration = endpoint_diagnostics_configuration_state(endpoint);
  sip_control = endpoint_diagnostics_sip_control_state(endpoint);
  media = endpoint_diagnostics_media_state(endpoint);
  tls_profile_policy = endpoint_diagnostics_tls_profile_policy_state(endpoint);
  /* Renderer health has no truthful runtime query.  Media readiness is also
   * deliberately not promoted from fixture/session state to a hardware pass. */
  length = snprintf((char *)response->data, response->capacity,
      "{\"state\":\"not_ready\",\"checks\":{"
      "\"configuration\":{\"state\":\"%s\"},"
      "\"sip_control\":{\"state\":\"%s\"},"
      "\"media_readiness\":{\"state\":\"%s\"},"
      "\"tls_profile_policy\":{\"state\":\"%s\"},"
      "\"renderer_truth\":{\"state\":\"unavailable\"}}}",
      configuration, sip_control, media, tls_profile_policy);
  if (length < 0 || (size_t)length >= response->capacity) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  response->length = (size_t)length;
  return AULA_STATUS_OK;
}

int endpoint_event_snapshot_request_is_valid(aula_bytes payload) {
  return payload.data != NULL && payload.length == 2U &&
      memcmp(payload.data, "{}", 2U) == 0;
}

aula_status endpoint_write_event_snapshot(aula_endpoint *endpoint,
                                           aula_mutable_bytes *response) {
  const char *state;
  int length;
  if (endpoint == NULL || response == NULL || response->data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  endpoint_refresh_status(endpoint);
  state = endpoint_call_state_name(endpoint->status.call_state);
  if (strcmp(state, "invalid") == 0) return AULA_STATUS_INTERNAL_ERROR;
  length = snprintf((char *)response->data, response->capacity,
      "{\"revision\":%llu,\"events\":[{"
      "\"id\":\"call-state-%llu\",\"type\":\"call_state\","
      "\"message\":\"%s\"}]}",
      (unsigned long long)endpoint->event_revision,
      (unsigned long long)endpoint->event_revision, state);
  if (length < 0 || (size_t)length >= response->capacity) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  response->length = (size_t)length;
  return AULA_STATUS_OK;
}

static aula_status endpoint_control_actions(aula_endpoint *endpoint,
                                             const aula_control_frame *request) {
  if (request->opcode == AULA_CONTROL_OPCODE_HANGUP) {
    return !aula_control_payload_is_empty_object(request->payload)
        ? AULA_STATUS_INVALID_DATA
        : aula_endpoint_command(endpoint, AULA_CONTROL_HANGUP);
  }
  if (request->opcode == AULA_CONTROL_OPCODE_ORIGINATE)
    return endpoint_control_originate(endpoint, request->payload);
  if (request->opcode == AULA_CONTROL_OPCODE_DTMF)
    return endpoint_control_dtmf(endpoint, request->payload);
  if (request->opcode == AULA_CONTROL_OPCODE_MEDIA)
    return endpoint_control_media(endpoint, request->payload);
  if (request->opcode == AULA_CONTROL_OPCODE_REPLACE_CREDENTIAL)
    return endpoint_control_replace_credential(endpoint, request->payload);
  return AULA_STATUS_UNSUPPORTED;
}

static aula_status endpoint_control_queries(aula_endpoint *endpoint,
                                             const aula_control_frame *request,
                                             aula_mutable_bytes *response) {
  if (request->opcode == AULA_CONTROL_OPCODE_DIAGNOSTICS) {
    return !endpoint_diagnostics_request_is_valid(request->payload)
        ? AULA_STATUS_INVALID_DATA
        : endpoint_write_diagnostics(endpoint, response);
  }
  if (request->opcode == AULA_CONTROL_OPCODE_SUBSCRIBE) {
    return !endpoint_event_snapshot_request_is_valid(request->payload)
        ? AULA_STATUS_INVALID_DATA
        : endpoint_write_event_snapshot(endpoint, response);
  }
  if (request->opcode == AULA_CONTROL_OPCODE_SETTINGS)
    return endpoint_control_settings(endpoint, request->payload, response);
  if (request->opcode == AULA_CONTROL_OPCODE_METRICS) {
    if (!aula_control_payload_is_empty_object(request->payload))
      return AULA_STATUS_INVALID_DATA;
    return endpoint_write_metrics(endpoint, response);
  }
  return AULA_STATUS_UNSUPPORTED;
}


static aula_status endpoint_control_request(void *context,
                                             const aula_control_frame *request,
                                             aula_mutable_bytes *response) {
  aula_endpoint *endpoint = (aula_endpoint *)context;
  aula_status status;
  if (endpoint == NULL || request == NULL || response == NULL) {
    return AULA_STATUS_INVALID_DATA;
  }
  response->length = 0U;
  status = endpoint_control_actions(endpoint, request);
  return status == AULA_STATUS_UNSUPPORTED
      ? endpoint_control_queries(endpoint, request, response) : status;
}

aula_status endpoint_create_control(aula_endpoint *endpoint) {
  aula_control_config config;
  if (endpoint == NULL || endpoint->view == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (endpoint->view->enable_local_control == 0) return AULA_STATUS_OK;
  (void)memset(&config, 0, sizeof(config));
  config.unix_socket_path = endpoint->view->local_control_socket;
  config.socket_mode = endpoint->view->local_control_socket_mode;
  config.maximum_request_bytes = endpoint->view->limits.control_message_bytes;
  config.maximum_clients = 8U;
  config.authorized_gateway_uid = endpoint->view->control_gateway_uid;
  config.command_callback = endpoint_control_command;
  config.request_callback = endpoint_control_request;
  config.command_context = endpoint;
  return aula_control_server_create(&config, &endpoint->control);
}

static aula_status endpoint_request_hangup(aula_endpoint *endpoint,
                                            aula_call_state state,
                                            aula_control_command command) {
  if (endpoint_state_is_inviting(state)) {
    return endpoint->dialog == NULL ? AULA_STATUS_STATE_ERROR :
        aula_sip_adapter_send_cancel(endpoint->dialog);
  }
  if (endpoint_state_has_media(state)) {
    return endpoint->dialog == NULL ? AULA_STATUS_STATE_ERROR :
        aula_sip_adapter_send_bye(endpoint->dialog);
  }
  if (state == AULA_CALL_BACKING_OFF && command == AULA_CONTROL_HANGUP) {
    return AULA_STATUS_OK;
  }
  if (endpoint_state_is_quiescent(state)) {
    if (command == AULA_CONTROL_SHUTDOWN) {
      endpoint_finish_shutdown(endpoint);
      endpoint_refresh_status(endpoint);
      return AULA_STATUS_END;
    }
    return AULA_STATUS_STATE_ERROR;
  }
  if (endpoint_state_is_stopped(state) || state == AULA_CALL_TERMINATING) {
    return AULA_STATUS_OK;
  }
  return AULA_STATUS_STATE_ERROR;
}

static aula_status endpoint_schedule_shutdown(aula_endpoint *endpoint,
                                               aula_call_state state) {
  uint64_t now;
  uint32_t grace_ms;
  if (endpoint_state_is_stopped(state)) {
    endpoint_finish_shutdown(endpoint);
    return AULA_STATUS_OK;
  }
  if (aula_platform_monotonic_now(&now) != AULA_STATUS_OK) {
    return AULA_STATUS_IO_ERROR;
  }
  grace_ms = endpoint->view->sip_transaction_timeout_ms;
  if (grace_ms == 0U || grace_ms > 5000U) grace_ms = 5000U;
  endpoint->shutdown_requested = 1;
  endpoint->shutdown_deadline_ns = endpoint_deadline_after(
      now, (uint64_t)grace_ms * UINT64_C(1000000));
  return AULA_STATUS_OK;
}

aula_status aula_endpoint_command(aula_endpoint *endpoint,
                                    aula_control_command command) {
  aula_call_state state;
  aula_status status;
  if (endpoint == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (command == AULA_CONTROL_STATUS) {
    return aula_endpoint_get_status(endpoint, &endpoint->status);
  }
  if (command != AULA_CONTROL_HANGUP && command != AULA_CONTROL_SHUTDOWN) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  state = aula_call_get_state(endpoint->call);
  status = endpoint_request_hangup(endpoint, state, command);
  if (status == AULA_STATUS_END) return AULA_STATUS_OK;
  if (status == AULA_STATUS_OK && !endpoint_state_is_stopped(state)) {
    endpoint->invite_deadline_ns = 0U;
    status = aula_call_apply_event(endpoint->call,
                                    AULA_CALL_EVENT_LOCAL_HANGUP, NULL);
  }
  if (status == AULA_STATUS_OK && state == AULA_CALL_BACKING_OFF) {
    endpoint->retry_deadline_ns = 0U;
    aula_sip_adapter_clear_outbound_target(endpoint->adapter);
  }
  if (status == AULA_STATUS_OK &&
      (endpoint_state_is_inviting(state) || endpoint_state_has_media(state))) {
    endpoint_release_media(endpoint);
    endpoint_clear_dtmf_schedule(endpoint);
  }
  if (status == AULA_STATUS_OK && command == AULA_CONTROL_SHUTDOWN) {
    status = endpoint_schedule_shutdown(endpoint, state);
  }
  endpoint_refresh_status(endpoint);
  return status;
}

int endpoint_shutdown_due(const aula_endpoint *endpoint, uint64_t now) {
  aula_call_state state = aula_call_get_state(endpoint->call);
  return endpoint->shutdown_requested != 0 &&
      (endpoint_state_is_stopped(state) || endpoint->shutdown_deadline_ns == 0U ||
       now >= endpoint->shutdown_deadline_ns);
}

void endpoint_take_stop_signal(aula_endpoint *endpoint) {
  aula_status status;
  if (aula_platform_take_signal_request() != AULA_SIGNAL_STOP ||
      endpoint->shutdown_requested != 0) return;
  status = aula_endpoint_command(endpoint, AULA_CONTROL_SHUTDOWN);
  if (status != AULA_STATUS_OK) {
    endpoint->shutdown_requested = 1;
    endpoint->shutdown_deadline_ns = 0U;
  }
}
