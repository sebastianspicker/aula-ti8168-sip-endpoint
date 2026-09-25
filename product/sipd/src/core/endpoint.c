#include "endpoint_internal.h"
#include "../media/session_private.h"
#include "../control/control_private.h"
#include "../sip/pjsip_readiness.h"
#include "../sip/adapter_internal.h"
#include "ls200_sipd/platform.h"

#include <stdlib.h>
#include <string.h>

#define LS200_ENDPOINT_IDLE_POLL_NS UINT64_C(20000000)

uint64_t endpoint_deadline_after(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void endpoint_consider_deadline(uint64_t candidate,
                                       uint64_t *deadline_ns) {
  if (candidate != 0U && candidate < *deadline_ns) *deadline_ns = candidate;
}

static uint64_t endpoint_service_deadline_ns(const ls200_endpoint *endpoint,
                                             uint64_t now) {
  uint64_t media_deadline;
  uint64_t deadline_ns = UINT64_MAX;
  if (endpoint == NULL) return deadline_ns;
  endpoint_consider_deadline(
      endpoint_deadline_after(now, LS200_ENDPOINT_IDLE_POLL_NS),
      &deadline_ns);
  endpoint_consider_deadline(endpoint->invite_deadline_ns,
                             &deadline_ns);
  endpoint_consider_deadline(endpoint->retry_deadline_ns,
                             &deadline_ns);
  endpoint_consider_deadline(endpoint->shutdown_deadline_ns,
                             &deadline_ns);
  if (endpoint->dtmf_end_packets_remaining != 0U ||
      endpoint->layout_next_digits_pending != 0U) {
    if (endpoint->dtmf_next_emit_ns < deadline_ns)
      deadline_ns = endpoint->dtmf_next_emit_ns;
  }
  media_deadline = ls200_media_session_next_action_ns(endpoint->media, now);
  if (media_deadline < deadline_ns) deadline_ns = media_deadline;
  return deadline_ns;
}

ls200_deadline endpoint_effective_poll_deadline(const ls200_endpoint *endpoint,
                                                uint64_t now,
                                                ls200_deadline requested) {
  uint64_t service_deadline = endpoint_service_deadline_ns(endpoint, now);
  if (service_deadline < requested.monotonic_ns)
    requested.monotonic_ns = service_deadline;
  return requested;
}

static void endpoint_record_poll_lateness(ls200_endpoint *endpoint,
                                          uint64_t service_deadline,
                                          uint64_t now) {
  uint64_t lateness;
  if (endpoint == NULL || service_deadline == UINT64_MAX ||
      now <= service_deadline)
    return;
  lateness = now - service_deadline;
  if (endpoint->poll_late_count != UINT64_MAX) ++endpoint->poll_late_count;
  if (lateness > endpoint->poll_max_lateness_ns)
    endpoint->poll_max_lateness_ns = lateness;
}

static ls200_status endpoint_capture_approved_domain(ls200_endpoint *endpoint) {
  ls200_sip_dial_target target;
  const char *uri;
  const char *host;
  size_t length;
  if (endpoint == NULL || endpoint->sip_config.outbound_uri == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  uri = endpoint->sip_config.outbound_uri;
  if (ls200_sip_parse_dial_target(uri, &target) != LS200_STATUS_OK)
    return LS200_STATUS_INVALID_DATA;
  host = strchr(uri + 4U, '@');
  if (host == NULL || host[1] == '\0') return LS200_STATUS_INVALID_DATA;
  ++host;
  length = strcspn(host, ":;");
  if (length == 0U || length >= sizeof(endpoint->approved_target_domain))
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(endpoint->approved_target_domain, host, length);
  endpoint->approved_target_domain[length] = '\0';
  endpoint->approved_target_port = target.has_explicit_port
      ? target.port
      : (endpoint->sip_config.preferred_transport == LS200_TRANSPORT_TLS
             ? 5061U : 5060U);
  return LS200_STATUS_OK;
}

ls200_status endpoint_create_adapter(ls200_endpoint *endpoint,
                                     const ls200_endpoint_options *options) {
  ls200_status status;
  endpoint->sip_config.network_scope_authorizer = endpoint_scope;
  endpoint->sip_config.network_scope_context = endpoint;
  endpoint->sip_config.event_callback = endpoint_sip_event;
  endpoint->sip_config.event_context = endpoint;
  status = ls200_sip_adapter_create(&endpoint->sip_config, &endpoint->adapter);
  if (status == LS200_STATUS_OK)
    ls200_sip_adapter_set_picture_fast_update_callback(
        endpoint->adapter, endpoint_handle_picture_fast_update, endpoint);
  if (status != LS200_STATUS_OK || options == NULL || options->sip_driver == NULL) {
    return status;
  }
  status = ls200_sip_driver_create(options->sip_driver, &endpoint->driver);
  if (status == LS200_STATUS_OK) {
    status = ls200_sip_adapter_attach_driver(endpoint->adapter, endpoint->driver);
  }
  return status;
}

ls200_status endpoint_begin_invite(ls200_endpoint *endpoint, int initial) {
  uint8_t offer_data[LS200_SIPD_MAX_SDP_BYTES];
  ls200_mutable_bytes offer = {offer_data, sizeof(offer_data), 0U};
  ls200_status status;
  if (endpoint == NULL || endpoint->driver == NULL) return LS200_STATUS_UNSUPPORTED;
  status = initial != 0 ? ls200_call_apply_event(endpoint->call,
                                                  LS200_CALL_EVENT_START, NULL) :
                          LS200_STATUS_OK;
  if (status == LS200_STATUS_OK) status = endpoint_prepare_offer(endpoint, &offer);
  if (status == LS200_STATUS_OK) {
    status = ls200_sip_adapter_start_invite(endpoint->adapter,
        (ls200_bytes){offer.data, offer.length}, &endpoint->dialog);
  }
  if (status == LS200_STATUS_OK &&
      ls200_call_get_state(endpoint->call) == LS200_CALL_RESOLVING) {
    status = ls200_call_apply_event(endpoint->call, LS200_CALL_EVENT_RESOLVED, NULL);
  }
  if (status == LS200_STATUS_OK) {
    uint64_t now;
    if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) {
      status = LS200_STATUS_IO_ERROR;
    } else {
      endpoint->invite_deadline_ns = endpoint_deadline_after(
          now, (uint64_t)endpoint->view->sip_transaction_timeout_ms *
                   UINT64_C(1000000));
    }
  }
  if (status != LS200_STATUS_OK) {
    endpoint->last_endpoint_error = status;
    if (initial != 0) {
      (void)ls200_call_apply_event(endpoint->call,
                                   LS200_CALL_EVENT_TRANSIENT_FAILURE, NULL);
      (void)ls200_call_apply_event(endpoint->call,
                                   LS200_CALL_EVENT_LOCAL_HANGUP, NULL);
    }
    endpoint->invite_deadline_ns = 0U;
    endpoint_release_media(endpoint);
    endpoint_retire_dialog(endpoint);
  }
  (void)memset(offer_data, 0, sizeof(offer_data));
  return status;
}

static ls200_status endpoint_load_configuration(ls200_endpoint *endpoint,
                                                const ls200_config *config,
                                                ls200_log_config *log_config) {
  ls200_status status = ls200_config_create_call_snapshot(config, &endpoint->snapshot);
  if (status != LS200_STATUS_OK) return status;
  endpoint->view = ls200_call_config_get_view(endpoint->snapshot);
  status = ls200_call_config_get_runtime_options(endpoint->snapshot,
      &endpoint->backend_config, &endpoint->sip_config, &endpoint->addresses);
  if (status == LS200_STATUS_OK) status = endpoint_settings_load(endpoint);
  if (status != LS200_STATUS_OK) return status;
  status = endpoint_capture_approved_domain(endpoint);
  return status == LS200_STATUS_OK
      ? ls200_config_get_log_config(config, log_config) : status;
}

static void endpoint_apply_options(ls200_endpoint *endpoint,
                                   const ls200_endpoint_options *options) {
  if (options == NULL) return;
  endpoint->media_target_authorizer = options->media_target_authorizer;
  endpoint->media_target_context = options->media_target_context;
  endpoint->renderer = options->renderer;
  endpoint->sip_config.credential_provider = options->credential_provider;
  endpoint->sip_config.credential_context = options->credential_context;
}

static ls200_status endpoint_create_resources(ls200_endpoint *endpoint,
                                              const ls200_log_config *log_config,
                                              const ls200_endpoint_options *options) {
  ls200_status status = ls200_log_create(log_config, &endpoint->logger);
  if (status == LS200_STATUS_OK) status = ls200_call_create(&endpoint->call);
  if (status == LS200_STATUS_OK) status = endpoint_create_adapter(endpoint, options);
  if (status == LS200_STATUS_OK) status = endpoint_create_control(endpoint);
  return status;
}

ls200_status ls200_endpoint_create(const ls200_config *config,
                                   const ls200_endpoint_options *options,
                                   ls200_endpoint **out_endpoint) {
  ls200_endpoint *endpoint;
  ls200_log_config log_config;
  ls200_status status;
  if (config == NULL || out_endpoint == NULL || *out_endpoint != NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  endpoint = (ls200_endpoint *)calloc(1U, sizeof(*endpoint));
  if (endpoint == NULL) return LS200_STATUS_INTERNAL_ERROR;
  status = endpoint_load_configuration(endpoint, config, &log_config);
  if (status == LS200_STATUS_OK) endpoint_apply_options(endpoint, options);
  if (status == LS200_STATUS_OK)
    status = endpoint_create_resources(endpoint, &log_config, options);
  if (status != LS200_STATUS_OK) {
    ls200_endpoint_destroy(endpoint);
    return status;
  }
  endpoint_refresh_status(endpoint);
  endpoint_log(endpoint, LS200_LOG_NOTICE, "created", "local_ready");
  *out_endpoint = endpoint;
  return LS200_STATUS_OK;
}

ls200_status ls200_endpoint_start(ls200_endpoint *endpoint) {
  ls200_status status;
  if (endpoint == NULL || endpoint->started != 0) return LS200_STATUS_STATE_ERROR;
  if (endpoint->view->enable_public_network == 0 || endpoint->driver == NULL) {
    return LS200_STATUS_UNSUPPORTED;
  }
  status = endpoint_begin_invite(endpoint, 1);
  if (status == LS200_STATUS_OK) endpoint->started = 1;
  endpoint_refresh_status(endpoint);
  return status;
}

static int endpoint_can_originate(const ls200_endpoint *endpoint) {
  ls200_call_state state;
  if (endpoint == NULL || endpoint->dialog != NULL || endpoint->media != NULL ||
      endpoint->pending_media != NULL) return 0;
  state = ls200_call_get_state(endpoint->call);
  return state == LS200_CALL_IDLE || state == LS200_CALL_TERMINATED;
}

static int endpoint_invite_deadline_due(const ls200_endpoint *endpoint,
                                        uint64_t now) {
  ls200_call_state state;
  if (endpoint->invite_deadline_ns == 0U || now < endpoint->invite_deadline_ns)
    return 0;
  state = ls200_call_get_state(endpoint->call);
  return state == LS200_CALL_INVITING || state == LS200_CALL_EARLY;
}

static void endpoint_expire_invite(ls200_endpoint *endpoint) {
  endpoint->invite_deadline_ns = 0U;
  if (endpoint->dialog != NULL) {
    (void)ls200_sip_adapter_send_cancel(endpoint->dialog);
  }
  endpoint_retry(endpoint, 0U);
}

static ls200_status endpoint_reset_terminated_call(ls200_endpoint *endpoint) {
  ls200_call *replacement = NULL;
  if (ls200_call_get_state(endpoint->call) != LS200_CALL_TERMINATED)
    return LS200_STATUS_OK;
  if (ls200_call_create(&replacement) != LS200_STATUS_OK)
    return LS200_STATUS_INTERNAL_ERROR;
  ls200_call_destroy(endpoint->call);
  endpoint->call = replacement;
  endpoint->started = 0;
  endpoint->invite_deadline_ns = 0U;
  endpoint->retry_deadline_ns = 0U;
  endpoint->shutdown_deadline_ns = 0U;
  endpoint->shutdown_requested = 0;
  return LS200_STATUS_OK;
}

static ls200_status endpoint_settings_allow_originate(
    const ls200_endpoint *endpoint) {
  return endpoint->settings_media_managed != 0
      ? LS200_STATUS_OK : LS200_STATUS_PERMISSION_DENIED;
}

static ls200_status endpoint_finish_failed_retry(ls200_endpoint *endpoint,
                                                 ls200_status failure) {
  ls200_status status;
  endpoint->last_endpoint_error = failure;
  endpoint->invite_deadline_ns = 0U;
  endpoint->retry_deadline_ns = 0U;
  ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
  status = ls200_call_apply_event(endpoint->call,
                                  LS200_CALL_EVENT_TRANSIENT_FAILURE, NULL);
  if (status == LS200_STATUS_OK) {
    status = ls200_call_apply_event(endpoint->call,
                                    LS200_CALL_EVENT_LOCAL_HANGUP, NULL);
  }
  endpoint_refresh_status(endpoint);
  return status == LS200_STATUS_OK ? LS200_STATUS_AGAIN : status;
}

ls200_status ls200_endpoint_originate(
    ls200_endpoint *endpoint, const ls200_zoom_dial_request *request) {
  ls200_zoom_dial_request bounded;
  uint8_t target_storage[LS200_ZOOM_MAX_TARGET_URI_BYTES];
  ls200_mutable_bytes target = {target_storage, sizeof(target_storage), 0U};
  ls200_status status;
  if (endpoint == NULL || request == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (endpoint->view->enable_public_network == 0 || endpoint->driver == NULL)
    return LS200_STATUS_UNSUPPORTED;
  status = endpoint_settings_allow_originate(endpoint);
  if (status != LS200_STATUS_OK) return status;
  if (!endpoint_can_originate(endpoint)) return LS200_STATUS_STATE_ERROR;
  if (request->profile != endpoint->sip_config.profile)
    return LS200_STATUS_SECURITY_ERROR;
  endpoint->last_endpoint_error = LS200_STATUS_OK;
  bounded = *request;
  if (bounded.profile != LS200_ZOOM_PROFILE_DIRECT_CRC)
    bounded.target_domain = endpoint->approved_target_domain;
  bounded.target_port = endpoint->approved_target_port;
  bounded.has_target_port = 1;
  status = ls200_zoom_build_dial_target(&bounded, &target);
  if (status == LS200_STATUS_OK)
    status = ls200_sip_adapter_set_outbound_target(
        endpoint->adapter, (const char *)target.data);
  if (status == LS200_STATUS_OK) status = endpoint_reset_terminated_call(endpoint);
  if (status == LS200_STATUS_OK) status = endpoint_begin_invite(endpoint, 1);
  if (status == LS200_STATUS_OK) endpoint->started = 1;
  else ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
  (void)memset(target_storage, 0, sizeof(target_storage));
  endpoint_refresh_status(endpoint);
  return status;
}

ls200_status ls200_endpoint_poll(ls200_endpoint *endpoint, ls200_deadline deadline) {
  int media_descriptors[4];
  size_t media_descriptor_count;
  uint64_t now;
  uint64_t service_deadline;
  ls200_deadline effective;
  ls200_status status;
  if (endpoint == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK)
    return LS200_STATUS_IO_ERROR;
  service_deadline = endpoint_service_deadline_ns(endpoint, now);
  effective = deadline;
  if (service_deadline < effective.monotonic_ns)
    effective.monotonic_ns = service_deadline;
  endpoint_take_stop_signal(endpoint);
  media_descriptor_count = ls200_media_session_get_descriptors_internal(
      endpoint->media, media_descriptors,
      sizeof(media_descriptors) / sizeof(media_descriptors[0]));
  (void)ls200_pjsip_media_readiness_sync(media_descriptors,
                                         media_descriptor_count);
  if (endpoint->control != NULL) {
    (void)ls200_control_server_poll_with_readiness(
        endpoint->control, effective, media_descriptors,
        media_descriptor_count);
  }
  status = ls200_sip_adapter_poll(endpoint->adapter, effective);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  endpoint_poll_media(endpoint, effective);
  if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) {
    return LS200_STATUS_IO_ERROR;
  }
  /* Caller deadlines only bound how long this invocation may wait. Lateness
   * is measured against the earliest internal service deadline captured at
   * entry, so an earlier caller deadline cannot create a false late sample. */
  endpoint_record_poll_lateness(endpoint, service_deadline, now);
  endpoint_poll_dtmf(endpoint, now);
  if (endpoint_shutdown_due(endpoint, now)) {
    endpoint_finish_shutdown(endpoint);
    endpoint_refresh_status(endpoint);
    return LS200_STATUS_END;
  }
  if (endpoint->retry_deadline_ns != 0U && now >= endpoint->retry_deadline_ns) {
    endpoint->retry_deadline_ns = 0U;
    if (ls200_call_apply_event(endpoint->call, LS200_CALL_EVENT_BACKOFF_ELAPSED,
                               NULL) == LS200_STATUS_OK) {
      status = endpoint_begin_invite(endpoint, 0);
      if (status != LS200_STATUS_OK)
        return endpoint_finish_failed_retry(endpoint, status);
      return status;
    }
  }
  if (endpoint_invite_deadline_due(endpoint, now)) endpoint_expire_invite(endpoint);
  endpoint_refresh_status(endpoint);
  return LS200_STATUS_AGAIN;
}

ls200_status ls200_endpoint_get_status(const ls200_endpoint *endpoint,
                                       ls200_endpoint_status *out_status) {
  if (endpoint == NULL || out_status == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_status = endpoint->status;
  return LS200_STATUS_OK;
}

void ls200_endpoint_destroy(ls200_endpoint *endpoint) {
  if (endpoint == NULL) return;
  ls200_pjsip_media_readiness_cancel();
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  endpoint_retire_dialog(endpoint);
  ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
  if (endpoint->renderer != NULL && endpoint->renderer->vtable != NULL &&
      endpoint->renderer->vtable->release != NULL) {
    endpoint->renderer->vtable->release(endpoint->renderer);
  }
  if (endpoint->call != NULL) {
    (void)ls200_call_apply_event(endpoint->call, LS200_CALL_EVENT_SHUTDOWN, NULL);
  }
  ls200_control_server_destroy(endpoint->control);
  ls200_sip_adapter_destroy(endpoint->adapter);
  ls200_sip_driver_destroy(endpoint->driver);
  endpoint_log(endpoint, LS200_LOG_NOTICE, "destroyed", "orderly_stop");
  ls200_call_destroy(endpoint->call);
  ls200_log_destroy(endpoint->logger);
  ls200_call_config_destroy(endpoint->snapshot);
  (void)memset(endpoint, 0, sizeof(*endpoint));
  free(endpoint);
}
