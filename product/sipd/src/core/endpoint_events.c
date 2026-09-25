#include "endpoint_internal.h"
#include "../sdp/negotiated_private.h"
#include "../sip/pjsip_readiness.h"

#include <stdio.h>
#include <string.h>

static int matches_client_final(const ls200_sip_event *event,
                                ls200_sip_method method) {
  return event != NULL && event->kind == LS200_SIP_EVENT_FINAL_RESPONSE &&
         event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == method &&
         event->response.cseq_method == method &&
         event->transaction.cseq_number != 0U &&
         event->response.cseq_number == event->transaction.cseq_number;
}

static ls200_status finish_bye(ls200_endpoint *endpoint) {
  if (ls200_call_get_state(endpoint->call) == LS200_CALL_TERMINATING) {
    endpoint_release_pending_media(endpoint);
    endpoint_release_media(endpoint);
    endpoint_retire_dialog(endpoint);
    ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
    return ls200_call_apply_event(endpoint->call,
                                  LS200_CALL_EVENT_TERMINATION_COMPLETE, NULL);
  }
  if (ls200_call_get_state(endpoint->call) == LS200_CALL_FAILED)
    endpoint_retry(endpoint, 0U);
  return LS200_STATUS_OK;
}

static ls200_status finish_late_invite(ls200_endpoint *endpoint,
                                       const ls200_sip_event *event) {
  ls200_status bye_status = LS200_STATUS_STATE_ERROR;
  if (event->response.status_code < 300U && endpoint->dialog != NULL)
    bye_status = ls200_sip_adapter_send_bye(endpoint->dialog);
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  if (event->response.status_code < 300U && bye_status == LS200_STATUS_OK)
    return LS200_STATUS_OK;
  endpoint_retire_dialog(endpoint);
  ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
  return ls200_call_apply_event(endpoint->call,
                                LS200_CALL_EVENT_TERMINATION_COMPLETE, NULL);
}

/* Only fixed stage labels and numeric status cross this diagnostic path. */
static void log_accept_failure(ls200_endpoint *endpoint, const char *reason,
                                ls200_status status) {
  endpoint_log(endpoint, LS200_LOG_WARNING, "invite_accept_failed", reason);
  (void)fprintf(stderr, "ls200-sipd: accept-stage=%s code=%d\n",
                 reason, (int)status);
  (void)fflush(stderr);
}

static ls200_status accept_invite(ls200_endpoint *endpoint,
                                  const ls200_sip_event *event) {
  ls200_sdp_negotiated_session *negotiated = NULL;
  ls200_status status = ls200_call_apply_event(
      endpoint->call, LS200_CALL_EVENT_ACCEPTED, NULL);
  if (status == LS200_STATUS_OK && event->response.has_sdp != 0) {
    status = endpoint->sip_config.profile == LS200_ZOOM_PROFILE_DIRECT_CRC ?
        ls200_sdp_negotiate_remote_answer_direct_crc(
            endpoint->offer, event->response.session_description, &negotiated) :
        ls200_sdp_negotiate_remote_answer(
            endpoint->offer, event->response.session_description, &negotiated);
    if (status != LS200_STATUS_OK) {
      log_accept_failure(endpoint, "sdp_negotiation_failed", status);
    }
  } else if (status == LS200_STATUS_OK) {
    status = LS200_STATUS_INVALID_DATA;
    log_accept_failure(endpoint, "missing_sdp_answer", status);
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_media_session_prepare(endpoint->media, negotiated);
    if (status != LS200_STATUS_OK) {
      log_accept_failure(endpoint, "media_prepare_failed", status);
    }
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_media_session_commit(endpoint->media);
    if (status != LS200_STATUS_OK) {
      log_accept_failure(endpoint, "media_commit_failed", status);
    }
  }
  ls200_sdp_negotiated_session_destroy(negotiated);
  if (status == LS200_STATUS_OK)
    status = ls200_call_apply_event(endpoint->call,
                                    LS200_CALL_EVENT_MEDIA_READY, NULL);
  if (status != LS200_STATUS_OK) endpoint_media_failure(endpoint);
  return status;
}

static ls200_status finish_invite(ls200_endpoint *endpoint,
                                  const ls200_sip_event *event) {
  endpoint->invite_deadline_ns = 0U;
  if (ls200_call_get_state(endpoint->call) == LS200_CALL_TERMINATING)
    return finish_late_invite(endpoint, event);
  if (event->response.status_code < 300U && event->is_duplicate == 0)
    return accept_invite(endpoint, event);
  if (event->response.status_code >= 300U &&
      event->response.final_classification == LS200_SIP_FINAL_TRANSIENT) {
    endpoint_retry(endpoint, event->response.retry_after_seconds);
    return LS200_STATUS_OK;
  }
  if (event->response.status_code >= 300U) {
    endpoint_release_media(endpoint);
    endpoint_retire_dialog(endpoint);
    ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
    return ls200_call_apply_event(endpoint->call,
                                  LS200_CALL_EVENT_PERMANENT_FAILURE, NULL);
  }
  return LS200_STATUS_OK;
}

static ls200_status final_response(ls200_endpoint *endpoint,
                                   const ls200_sip_event *event) {
  if (matches_client_final(event, LS200_SIP_METHOD_BYE))
    return finish_bye(endpoint);
  if (matches_client_final(event, LS200_SIP_METHOD_CANCEL))
    return LS200_STATUS_OK;
  if (matches_client_final(event, LS200_SIP_METHOD_INVITE))
    return finish_invite(endpoint, event);
  return LS200_STATUS_OK;
}

static ls200_status remote_ack(ls200_endpoint *endpoint,
                               const ls200_sip_event *event) {
  ls200_status status;
  if (endpoint->reinvite_pending == 0 ||
      event->transaction.role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
      event->transaction.method != LS200_SIP_METHOD_ACK ||
      event->transaction.cseq_number != endpoint->reinvite_cseq)
    return LS200_STATUS_OK;
  status = ls200_media_session_commit(endpoint->pending_media);
  if (status == LS200_STATUS_OK) {
    ls200_pjsip_media_readiness_cancel();
    endpoint_destroy_media_session(&endpoint->media);
    endpoint->media = endpoint->pending_media;
    endpoint->pending_media = NULL;
    endpoint->reserved = endpoint->pending_reserved;
    (void)memset(&endpoint->pending_reserved, 0,
                 sizeof(endpoint->pending_reserved));
  } else {
    endpoint_release_pending_media(endpoint);
    return status;
  }
  endpoint->reinvite_pending = 0;
  ls200_sdp_negotiated_session_destroy(endpoint->staged_reinvite);
  endpoint->staged_reinvite = NULL;
  endpoint->reinvite_cseq = 0U;
  return status;
}

static ls200_status remote_hangup(ls200_endpoint *endpoint) {
  ls200_status status;
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  status = ls200_call_apply_event(endpoint->call,
                                  LS200_CALL_EVENT_REMOTE_HANGUP, NULL);
  endpoint_retire_dialog(endpoint);
  ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
  return status;
}

static int is_retry_event(ls200_sip_event_kind kind) {
  return kind == LS200_SIP_EVENT_TRANSPORT_ERROR ||
         kind == LS200_SIP_EVENT_TRANSACTION_TIMEOUT;
}

static ls200_status dispatch_resolved_peer(ls200_endpoint *endpoint) {
  return ls200_call_get_state(endpoint->call) == LS200_CALL_RESOLVING
             ? ls200_call_apply_event(endpoint->call,
                                      LS200_CALL_EVENT_RESOLVED, NULL)
             : LS200_STATUS_OK;
}

static ls200_status dispatch_provisional(ls200_endpoint *endpoint,
                                         const ls200_sip_event *event) {
  return event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
                 event->transaction.method == LS200_SIP_METHOD_INVITE
             ? ls200_call_apply_event(endpoint->call,
                                      LS200_CALL_EVENT_PROVISIONAL, NULL)
             : LS200_STATUS_OK;
}

static int is_remote_hangup_event(ls200_sip_event_kind kind) {
  return kind == LS200_SIP_EVENT_REMOTE_BYE ||
         kind == LS200_SIP_EVENT_REMOTE_CANCEL ||
         kind == LS200_SIP_EVENT_SESSION_TERMINATED;
}

static ls200_status dispatch_remaining_event(ls200_endpoint *endpoint,
                                             const ls200_sip_event *event) {
  switch (event->kind) {
    case LS200_SIP_EVENT_REINVITE:
      return endpoint_stage_reinvite(endpoint, event);
    case LS200_SIP_EVENT_REMOTE_ACK:
      return remote_ack(endpoint, event);
    case LS200_SIP_EVENT_DIGEST_CHALLENGE:
      return endpoint->dialog == NULL
                 ? LS200_STATUS_STATE_ERROR
                 : ls200_sip_adapter_continue_digest(endpoint->dialog);
    case LS200_SIP_EVENT_TRANSPORT_FALLBACK:
      return LS200_STATUS_OK;
    default:
      return LS200_STATUS_INVALID_DATA;
  }
}

static ls200_status dispatch_event(ls200_endpoint *endpoint,
                                   const ls200_sip_event *event) {
  if (is_retry_event(event->kind)) {
    if (ls200_call_get_state(endpoint->call) == LS200_CALL_TERMINATING) {
      endpoint->invite_deadline_ns = 0U;
      endpoint_release_pending_media(endpoint);
      endpoint_release_media(endpoint);
      endpoint_retire_dialog(endpoint);
      ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
      return ls200_call_apply_event(endpoint->call,
                                    LS200_CALL_EVENT_TERMINATION_COMPLETE,
                                    NULL);
    }
    endpoint_retry(endpoint, 0U);
    return LS200_STATUS_OK;
  }
  if (event->kind == LS200_SIP_EVENT_RESOLVED_PEER)
    return dispatch_resolved_peer(endpoint);
  if (event->kind == LS200_SIP_EVENT_PROVISIONAL)
    return dispatch_provisional(endpoint, event);
  if (event->kind == LS200_SIP_EVENT_FINAL_RESPONSE)
    return final_response(endpoint, event);
  if (is_remote_hangup_event(event->kind)) return remote_hangup(endpoint);
  return dispatch_remaining_event(endpoint, event);
}

void endpoint_sip_event(void *context, const ls200_sip_event *event) {
  ls200_endpoint *endpoint = (ls200_endpoint *)context;
  ls200_status status;
  if (endpoint == NULL || event == NULL) return;
  status = dispatch_event(endpoint, event);
  if (status != LS200_STATUS_OK) {
    endpoint->last_endpoint_error = status;
    endpoint_log(endpoint, LS200_LOG_WARNING, "event_failure",
                 "transaction_rejected");
  }
  endpoint_refresh_status(endpoint);
}
