#include "endpoint_internal.h"
#include "../sdp/negotiated_private.h"
#include "../sip/pjsip_readiness.h"

#include <stdio.h>
#include <string.h>

static int matches_client_final(const aula_sip_event *event,
                                aula_sip_method method) {
  return event != NULL && event->kind == AULA_SIP_EVENT_FINAL_RESPONSE &&
         event->transaction.role == AULA_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == method &&
         event->response.cseq_method == method &&
         event->transaction.cseq_number != 0U &&
         event->response.cseq_number == event->transaction.cseq_number;
}

static aula_status finish_bye(aula_endpoint *endpoint) {
  if (aula_call_get_state(endpoint->call) == AULA_CALL_TERMINATING) {
    endpoint_release_pending_media(endpoint);
    endpoint_release_media(endpoint);
    endpoint_retire_dialog(endpoint);
    aula_sip_adapter_clear_outbound_target(endpoint->adapter);
    return aula_call_apply_event(endpoint->call,
                                  AULA_CALL_EVENT_TERMINATION_COMPLETE, NULL);
  }
  if (aula_call_get_state(endpoint->call) == AULA_CALL_FAILED)
    endpoint_retry(endpoint, 0U);
  return AULA_STATUS_OK;
}

static aula_status finish_late_invite(aula_endpoint *endpoint,
                                       const aula_sip_event *event) {
  aula_status bye_status = AULA_STATUS_STATE_ERROR;
  if (event->response.status_code < 300U && endpoint->dialog != NULL)
    bye_status = aula_sip_adapter_send_bye(endpoint->dialog);
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  if (event->response.status_code < 300U && bye_status == AULA_STATUS_OK)
    return AULA_STATUS_OK;
  endpoint_retire_dialog(endpoint);
  aula_sip_adapter_clear_outbound_target(endpoint->adapter);
  return aula_call_apply_event(endpoint->call,
                                AULA_CALL_EVENT_TERMINATION_COMPLETE, NULL);
}

/* Only fixed stage labels and numeric status cross this diagnostic path. */
static void log_accept_failure(aula_endpoint *endpoint, const char *reason,
                                aula_status status) {
  endpoint_log(endpoint, AULA_LOG_WARNING, "invite_accept_failed", reason);
  (void)fprintf(stderr, "aula-sipd: accept-stage=%s code=%d\n",
                 reason, (int)status);
  (void)fflush(stderr);
}

static aula_status accept_invite(aula_endpoint *endpoint,
                                  const aula_sip_event *event) {
  aula_sdp_negotiated_session *negotiated = NULL;
  aula_status status = aula_call_apply_event(
      endpoint->call, AULA_CALL_EVENT_ACCEPTED, NULL);
  if (status == AULA_STATUS_OK && event->response.has_sdp != 0) {
    status = endpoint->sip_config.profile == AULA_ZOOM_PROFILE_DIRECT_CRC ?
        aula_sdp_negotiate_remote_answer_direct_crc(
            endpoint->offer, event->response.session_description, &negotiated) :
        aula_sdp_negotiate_remote_answer(
            endpoint->offer, event->response.session_description, &negotiated);
    if (status != AULA_STATUS_OK) {
      log_accept_failure(endpoint, "sdp_negotiation_failed", status);
    }
  } else if (status == AULA_STATUS_OK) {
    status = AULA_STATUS_INVALID_DATA;
    log_accept_failure(endpoint, "missing_sdp_answer", status);
  }
  if (status == AULA_STATUS_OK) {
    status = aula_media_session_prepare(endpoint->media, negotiated);
    if (status != AULA_STATUS_OK) {
      log_accept_failure(endpoint, "media_prepare_failed", status);
    }
  }
  if (status == AULA_STATUS_OK) {
    status = aula_media_session_commit(endpoint->media);
    if (status != AULA_STATUS_OK) {
      log_accept_failure(endpoint, "media_commit_failed", status);
    }
  }
  aula_sdp_negotiated_session_destroy(negotiated);
  if (status == AULA_STATUS_OK)
    status = aula_call_apply_event(endpoint->call,
                                    AULA_CALL_EVENT_MEDIA_READY, NULL);
  if (status != AULA_STATUS_OK) endpoint_media_failure(endpoint);
  return status;
}

static aula_status finish_invite(aula_endpoint *endpoint,
                                  const aula_sip_event *event) {
  endpoint->invite_deadline_ns = 0U;
  if (aula_call_get_state(endpoint->call) == AULA_CALL_TERMINATING)
    return finish_late_invite(endpoint, event);
  if (event->response.status_code < 300U && event->is_duplicate == 0)
    return accept_invite(endpoint, event);
  if (event->response.status_code >= 300U &&
      event->response.final_classification == AULA_SIP_FINAL_TRANSIENT) {
    endpoint_retry(endpoint, event->response.retry_after_seconds);
    return AULA_STATUS_OK;
  }
  if (event->response.status_code >= 300U) {
    endpoint_release_media(endpoint);
    endpoint_retire_dialog(endpoint);
    aula_sip_adapter_clear_outbound_target(endpoint->adapter);
    return aula_call_apply_event(endpoint->call,
                                  AULA_CALL_EVENT_PERMANENT_FAILURE, NULL);
  }
  return AULA_STATUS_OK;
}

static aula_status final_response(aula_endpoint *endpoint,
                                   const aula_sip_event *event) {
  if (matches_client_final(event, AULA_SIP_METHOD_BYE))
    return finish_bye(endpoint);
  if (matches_client_final(event, AULA_SIP_METHOD_CANCEL))
    return AULA_STATUS_OK;
  if (matches_client_final(event, AULA_SIP_METHOD_INVITE))
    return finish_invite(endpoint, event);
  return AULA_STATUS_OK;
}

static aula_status remote_ack(aula_endpoint *endpoint,
                               const aula_sip_event *event) {
  aula_status status;
  if (endpoint->reinvite_pending == 0 ||
      event->transaction.role != AULA_SIP_TRANSACTION_ROLE_SERVER ||
      event->transaction.method != AULA_SIP_METHOD_ACK ||
      event->transaction.cseq_number != endpoint->reinvite_cseq)
    return AULA_STATUS_OK;
  status = aula_media_session_commit(endpoint->pending_media);
  if (status == AULA_STATUS_OK) {
    aula_pjsip_media_readiness_cancel();
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
  aula_sdp_negotiated_session_destroy(endpoint->staged_reinvite);
  endpoint->staged_reinvite = NULL;
  endpoint->reinvite_cseq = 0U;
  return status;
}

static aula_status remote_hangup(aula_endpoint *endpoint) {
  aula_status status;
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  status = aula_call_apply_event(endpoint->call,
                                  AULA_CALL_EVENT_REMOTE_HANGUP, NULL);
  endpoint_retire_dialog(endpoint);
  aula_sip_adapter_clear_outbound_target(endpoint->adapter);
  return status;
}

static int is_retry_event(aula_sip_event_kind kind) {
  return kind == AULA_SIP_EVENT_TRANSPORT_ERROR ||
         kind == AULA_SIP_EVENT_TRANSACTION_TIMEOUT;
}

static aula_status dispatch_resolved_peer(aula_endpoint *endpoint) {
  return aula_call_get_state(endpoint->call) == AULA_CALL_RESOLVING
             ? aula_call_apply_event(endpoint->call,
                                      AULA_CALL_EVENT_RESOLVED, NULL)
             : AULA_STATUS_OK;
}

static aula_status dispatch_provisional(aula_endpoint *endpoint,
                                         const aula_sip_event *event) {
  return event->transaction.role == AULA_SIP_TRANSACTION_ROLE_CLIENT &&
                 event->transaction.method == AULA_SIP_METHOD_INVITE
             ? aula_call_apply_event(endpoint->call,
                                      AULA_CALL_EVENT_PROVISIONAL, NULL)
             : AULA_STATUS_OK;
}

static int is_remote_hangup_event(aula_sip_event_kind kind) {
  return kind == AULA_SIP_EVENT_REMOTE_BYE ||
         kind == AULA_SIP_EVENT_REMOTE_CANCEL ||
         kind == AULA_SIP_EVENT_SESSION_TERMINATED;
}

static aula_status dispatch_remaining_event(aula_endpoint *endpoint,
                                             const aula_sip_event *event) {
  switch (event->kind) {
    case AULA_SIP_EVENT_REINVITE:
      return endpoint_stage_reinvite(endpoint, event);
    case AULA_SIP_EVENT_REMOTE_ACK:
      return remote_ack(endpoint, event);
    case AULA_SIP_EVENT_DIGEST_CHALLENGE:
      return endpoint->dialog == NULL
                 ? AULA_STATUS_STATE_ERROR
                 : aula_sip_adapter_continue_digest(endpoint->dialog);
    case AULA_SIP_EVENT_TRANSPORT_FALLBACK:
      return AULA_STATUS_OK;
    default:
      return AULA_STATUS_INVALID_DATA;
  }
}

static aula_status dispatch_event(aula_endpoint *endpoint,
                                   const aula_sip_event *event) {
  if (is_retry_event(event->kind)) {
    if (aula_call_get_state(endpoint->call) == AULA_CALL_TERMINATING) {
      endpoint->invite_deadline_ns = 0U;
      endpoint_release_pending_media(endpoint);
      endpoint_release_media(endpoint);
      endpoint_retire_dialog(endpoint);
      aula_sip_adapter_clear_outbound_target(endpoint->adapter);
      return aula_call_apply_event(endpoint->call,
                                    AULA_CALL_EVENT_TERMINATION_COMPLETE,
                                    NULL);
    }
    endpoint_retry(endpoint, 0U);
    return AULA_STATUS_OK;
  }
  if (event->kind == AULA_SIP_EVENT_RESOLVED_PEER)
    return dispatch_resolved_peer(endpoint);
  if (event->kind == AULA_SIP_EVENT_PROVISIONAL)
    return dispatch_provisional(endpoint, event);
  if (event->kind == AULA_SIP_EVENT_FINAL_RESPONSE)
    return final_response(endpoint, event);
  if (is_remote_hangup_event(event->kind)) return remote_hangup(endpoint);
  return dispatch_remaining_event(endpoint, event);
}

void endpoint_sip_event(void *context, const aula_sip_event *event) {
  aula_endpoint *endpoint = (aula_endpoint *)context;
  aula_status status;
  if (endpoint == NULL || event == NULL) return;
  status = dispatch_event(endpoint, event);
  if (status != AULA_STATUS_OK) {
    endpoint->last_endpoint_error = status;
    endpoint_log(endpoint, AULA_LOG_WARNING, "event_failure",
                 "transaction_rejected");
  }
  endpoint_refresh_status(endpoint);
}
