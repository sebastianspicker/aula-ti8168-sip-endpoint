#include "adapter_internal.h"

#include "driver_internal.h"

#include <string.h>

static int event_is_adapter_wide(ls200_sip_event_kind kind) {
  return kind == LS200_SIP_EVENT_RESOLVED_PEER ||
         kind == LS200_SIP_EVENT_TRANSPORT_FALLBACK;
}

static int event_is_native_session_terminal(ls200_sip_event_kind kind) {
  return kind == LS200_SIP_EVENT_SESSION_TERMINATED;
}

static int transaction_matches(
    const ls200_sip_transaction_identity *left,
    const ls200_sip_transaction_identity *right) {
  return ls200_sip_transaction_is_valid(left) &&
         ls200_sip_transaction_is_valid(right) && left->handle == right->handle &&
         left->role == right->role && left->method == right->method &&
         left->cseq_number == right->cseq_number;
}

static ls200_status copy_identifiers(ls200_sip_dialog *dialog,
                                     const ls200_sip_dialog_identifiers *ids) {
  if (ids == NULL) return LS200_STATUS_OK;
  if (!ls200_sip_text_is_safe(ids->call_id, sizeof(dialog->call_id)) ||
      !ls200_sip_text_is_safe(ids->local_tag, sizeof(dialog->local_tag)) ||
      !ls200_sip_text_is_safe(ids->remote_tag, sizeof(dialog->remote_tag)))
    return LS200_STATUS_INVALID_DATA;
  (void)ls200_sip_copy(dialog->call_id, sizeof(dialog->call_id), ids->call_id);
  (void)ls200_sip_copy(dialog->local_tag, sizeof(dialog->local_tag), ids->local_tag);
  (void)ls200_sip_copy(dialog->remote_tag, sizeof(dialog->remote_tag),
                       ids->remote_tag);
  return LS200_STATUS_OK;
}

static ls200_status copy_routes(ls200_sip_dialog *dialog,
                                const ls200_sip_route_set *routes) {
  size_t index;
  if (routes == NULL) return LS200_STATUS_OK;
  if (routes->count > LS200_SIP_MAX_ROUTE_COUNT)
    return LS200_STATUS_LIMIT_EXCEEDED;
  dialog->routes.count = routes->count;
  for (index = 0U; index < routes->count; ++index) {
    if (!ls200_sip_text_is_safe(routes->routes[index],
                                LS200_SIP_MAX_ROUTE_URI_BYTES))
      return LS200_STATUS_INVALID_DATA;
    (void)ls200_sip_copy(dialog->route_values[index],
                         sizeof(dialog->route_values[index]),
                         routes->routes[index]);
    dialog->routes.routes[index] = dialog->route_values[index];
  }
  return LS200_STATUS_OK;
}

static ls200_status copy_update(ls200_sip_dialog *dialog,
                                const ls200_sip_dialog_update *source) {
  ls200_status status;
  if (dialog == NULL || source == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = copy_identifiers(dialog, source->identifiers);
  if (status == LS200_STATUS_OK) status = copy_routes(dialog, source->routes);
  if (status != LS200_STATUS_OK) return status;
  if (ls200_sip_transport_is_valid(&source->peer))
    dialog->update.peer = source->peer;
  if (source->local_cseq != 0U) dialog->update.local_cseq = source->local_cseq;
  if (source->remote_cseq != 0U)
    dialog->update.remote_cseq = source->remote_cseq;
  return LS200_STATUS_OK;
}

static int challenge_fields_valid(
    const ls200_sip_digest_challenge *challenge) {
  return challenge != NULL &&
         ls200_sip_text_is_safe(challenge->realm,
                                LS200_SIP_MAX_DIGEST_FIELD_BYTES) &&
         ls200_sip_text_is_safe(challenge->nonce,
                                LS200_SIP_MAX_DIGEST_FIELD_BYTES) &&
         ls200_sip_text_is_safe(challenge->algorithm,
                                LS200_SIP_MAX_DIGEST_FIELD_BYTES) &&
         strcmp(challenge->algorithm, "SHA-256") == 0 &&
         challenge->qop != NULL &&
         ls200_sip_text_is_safe(challenge->qop,
                                LS200_SIP_MAX_DIGEST_FIELD_BYTES) &&
         strcmp(challenge->qop, "auth") == 0 &&
         (challenge->opaque == NULL ||
          ls200_sip_text_is_safe(challenge->opaque,
                                 LS200_SIP_MAX_DIGEST_FIELD_BYTES));
}

static int challenge_authority_matches(
    const ls200_sip_dialog *dialog,
    const ls200_sip_digest_challenge *challenge) {
  return strcmp(challenge->realm, dialog->realm) == 0 &&
         (challenge->proxy_challenge != 0) ==
             dialog->challenge.proxy_challenge &&
         strcmp(challenge->algorithm, dialog->algorithm) == 0 &&
         strcmp(challenge->qop, dialog->qop) == 0;
}

static void save_challenge(ls200_sip_dialog *dialog,
                           const ls200_sip_digest_challenge *challenge) {
  (void)ls200_sip_copy(dialog->realm, sizeof(dialog->realm), challenge->realm);
  (void)ls200_sip_copy(dialog->nonce, sizeof(dialog->nonce), challenge->nonce);
  if (challenge->opaque != NULL)
    (void)ls200_sip_copy(dialog->opaque, sizeof(dialog->opaque),
                         challenge->opaque);
  else
    dialog->opaque[0] = '\0';
  (void)ls200_sip_copy(dialog->algorithm, sizeof(dialog->algorithm),
                       challenge->algorithm);
  (void)ls200_sip_copy(dialog->qop, sizeof(dialog->qop), challenge->qop);
  dialog->challenge.realm = dialog->realm;
  dialog->challenge.nonce = dialog->nonce;
  dialog->challenge.opaque =
      dialog->opaque[0] == '\0' ? NULL : dialog->opaque;
  dialog->challenge.algorithm = dialog->algorithm;
  dialog->challenge.qop = dialog->qop[0] == '\0' ? NULL : dialog->qop;
  dialog->challenge.proxy_challenge = challenge->proxy_challenge != 0;
  dialog->digest_authority_pinned = 1;
  ++dialog->digest_challenge_count;
}

static ls200_status store_challenge(
    ls200_sip_dialog *dialog, const ls200_sip_digest_challenge *challenge) {
  if (dialog == NULL || !challenge_fields_valid(challenge))
    return LS200_STATUS_INVALID_DATA;
  if (dialog->digest_authority_pinned != 0 &&
      !challenge_authority_matches(dialog, challenge))
    return LS200_STATUS_SECURITY_ERROR;
  if (dialog->digest_authority_pinned != 0 &&
      strcmp(challenge->nonce, dialog->nonce) == 0)
    return LS200_STATUS_SECURITY_ERROR;
  if (dialog->digest_challenge_count >=
      dialog->adapter->endpoint.limits.maximum_digest_challenges_per_dialog)
    return LS200_STATUS_LIMIT_EXCEEDED;
  save_challenge(dialog, challenge);
  return LS200_STATUS_OK;
}

static int is_client_progress_kind(ls200_sip_event_kind kind) {
  return kind == LS200_SIP_EVENT_PROVISIONAL ||
         kind == LS200_SIP_EVENT_FINAL_RESPONSE ||
         kind == LS200_SIP_EVENT_DIGEST_CHALLENGE ||
         kind == LS200_SIP_EVENT_TRANSACTION_TIMEOUT ||
         kind == LS200_SIP_EVENT_TRANSPORT_ERROR;
}

static int is_cancelled_initial_invite_terminal(
    const ls200_sip_dialog *dialog, const ls200_sip_event *event) {
  return dialog != NULL && event != NULL && dialog->cancel_pending != 0 &&
         (event->kind == LS200_SIP_EVENT_TRANSACTION_TIMEOUT ||
          event->kind == LS200_SIP_EVENT_TRANSPORT_ERROR) &&
         event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == LS200_SIP_METHOD_INVITE &&
         event->transaction.cseq_number ==
             dialog->initial_invite_transaction.cseq_number;
}

static ls200_status validate_client_progress(const ls200_sip_dialog *dialog,
                                             const ls200_sip_event *event) {
  if (event->transaction.role != LS200_SIP_TRANSACTION_ROLE_CLIENT ||
      event->transaction.cseq_number != dialog->update.local_cseq)
    return LS200_STATUS_STATE_ERROR;
  if (event->transaction.method != dialog->outbound_method &&
      !is_cancelled_initial_invite_terminal(dialog, event))
    return LS200_STATUS_STATE_ERROR;
  if (dialog->active_client_transaction.handle != NULL &&
      !transaction_matches(&dialog->active_client_transaction,
                           &event->transaction))
    return LS200_STATUS_STATE_ERROR;
  if (event->kind == LS200_SIP_EVENT_PROVISIONAL &&
      (event->response.status_code < 100U ||
       event->response.status_code >= 200U))
    return LS200_STATUS_INVALID_DATA;
  if (event->kind == LS200_SIP_EVENT_FINAL_RESPONSE &&
      (event->response.status_code < 200U ||
       event->response.status_code > 699U))
    return LS200_STATUS_INVALID_DATA;
  return LS200_STATUS_OK;
}

static ls200_status validate_reinvite(const ls200_sip_event *event) {
  if (event->transaction.role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
      event->transaction.method != LS200_SIP_METHOD_REINVITE ||
      event->reinvite == NULL ||
      !transaction_matches(&event->transaction,
                           &event->reinvite->request_transaction))
    return LS200_STATUS_STATE_ERROR;
  return LS200_STATUS_OK;
}

static ls200_status validate_remote_event(const ls200_sip_dialog *dialog,
                                          const ls200_sip_event *event) {
  if (event->kind == LS200_SIP_EVENT_REMOTE_ACK &&
      (event->transaction.role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
       event->transaction.method != LS200_SIP_METHOD_ACK ||
       !dialog->inbound_reinvite_pending ||
       event->transaction.cseq_number != dialog->inbound_reinvite.cseq_number))
    return LS200_STATUS_STATE_ERROR;
  if (event->kind == LS200_SIP_EVENT_REMOTE_BYE &&
      (event->transaction.role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
       event->transaction.method != LS200_SIP_METHOD_BYE))
    return LS200_STATUS_STATE_ERROR;
  if (event->kind == LS200_SIP_EVENT_REMOTE_CANCEL &&
      (event->transaction.role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
       event->transaction.method != LS200_SIP_METHOD_CANCEL))
    return LS200_STATUS_STATE_ERROR;
  return LS200_STATUS_OK;
}

static int is_accepted_invite_duplicate(const ls200_sip_dialog *dialog,
                                        const ls200_sip_event *event) {
  return event->kind == LS200_SIP_EVENT_FINAL_RESPONSE &&
         event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == LS200_SIP_METHOD_INVITE &&
         event->response.status_code >= 200U &&
         event->response.status_code < 300U &&
         dialog->accepted_invite_transaction.handle != NULL;
}

static int is_cancelled_invite_final(const ls200_sip_dialog *dialog,
                                     const ls200_sip_event *event) {
  return event->kind == LS200_SIP_EVENT_FINAL_RESPONSE &&
         dialog->cancel_pending != 0 &&
         event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == LS200_SIP_METHOD_INVITE &&
         event->transaction.cseq_number ==
             dialog->initial_invite_transaction.cseq_number;
}

static ls200_status validate_dialog_event(const ls200_sip_dialog *dialog,
                                          const ls200_sip_event *event) {
  if (event == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (event_is_adapter_wide(event->kind)) return LS200_STATUS_OK;
  if (dialog == NULL || dialog->driver_dialog == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (event_is_native_session_terminal(event->kind))
    return event->reason_code == NULL ? LS200_STATUS_INVALID_DATA :
                                        LS200_STATUS_OK;
  if (!ls200_sip_transaction_is_valid(&event->transaction))
    return LS200_STATUS_INVALID_DATA;
  if (is_accepted_invite_duplicate(dialog, event))
    return transaction_matches(&dialog->accepted_invite_transaction,
                               &event->transaction)
               ? LS200_STATUS_OK
               : LS200_STATUS_STATE_ERROR;
  if (is_cancelled_invite_final(dialog, event))
    return dialog->initial_invite_transaction.handle == NULL ||
                   transaction_matches(&dialog->initial_invite_transaction,
                                       &event->transaction)
               ? LS200_STATUS_OK
               : LS200_STATUS_STATE_ERROR;
  if (is_client_progress_kind(event->kind))
    return validate_client_progress(dialog, event);
  if (event->kind == LS200_SIP_EVENT_REINVITE)
    return validate_reinvite(event);
  return validate_remote_event(dialog, event);
}

ls200_status ls200_sip_adapter_driver_sink(
    void *context, const ls200_sip_driver_event *driver_event) {
  ls200_sip_dialog *dialog;
  if (context == NULL || driver_event == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (!event_is_adapter_wide(driver_event->event.kind) &&
      driver_event->driver_dialog == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  dialog = driver_event->driver_dialog == NULL
               ? NULL
               : ls200_sip_find_dialog((ls200_sip_adapter *)context,
                                       driver_event->driver_dialog);
  if (driver_event->driver_dialog != NULL && dialog == NULL)
    return LS200_STATUS_END;
  return ls200_sip_adapter_ingest_driver_event(
      (ls200_sip_adapter *)context, dialog, &driver_event->event);
}

static ls200_status authorize_event(ls200_sip_adapter *adapter,
                                    ls200_sip_dialog *dialog,
                                    const ls200_sip_event *event) {
  if (event->kind == LS200_SIP_EVENT_RESOLVED_PEER)
    return ls200_sip_authorize_peer(adapter, &event->transport, 0);
  if (event->kind == LS200_SIP_EVENT_TRANSPORT_FALLBACK)
    return ls200_sip_authorize_peer(adapter, &event->transport, 1);
  if (event->kind == LS200_SIP_EVENT_DIGEST_CHALLENGE)
    return dialog == NULL ? LS200_STATUS_INVALID_ARGUMENT
                          : store_challenge(dialog, event->digest_challenge);
  return LS200_STATUS_OK;
}

static int fallback_peer_is_pinned_tcp(const ls200_sip_dialog *dialog,
                                       const ls200_sip_transport_info *peer) {
  return dialog != NULL && ls200_sip_transport_is_valid(peer) &&
         dialog->update.peer.transport == LS200_TRANSPORT_UDP &&
         peer->transport == LS200_TRANSPORT_TCP &&
         dialog->update.peer.remote_port == peer->remote_port &&
         dialog->update.peer.remote_address_length == peer->remote_address_length &&
         memcmp(dialog->update.peer.remote_address, peer->remote_address,
                peer->remote_address_length) == 0;
}

static void fallback_reseed_invite_transactions(
    ls200_sip_dialog *dialog, const ls200_sip_dialog_update *update) {
  if (update != NULL && update->local_cseq != 0U)
    dialog->update.local_cseq = update->local_cseq;
  (void)memset(&dialog->active_client_transaction, 0,
               sizeof(dialog->active_client_transaction));
  (void)memset(&dialog->initial_invite_transaction, 0,
               sizeof(dialog->initial_invite_transaction));
  dialog->initial_invite_transaction.role = LS200_SIP_TRANSACTION_ROLE_CLIENT;
  dialog->initial_invite_transaction.method = LS200_SIP_METHOD_INVITE;
  dialog->initial_invite_transaction.cseq_number = dialog->update.local_cseq;
}

static ls200_status apply_transport_fallback(ls200_sip_dialog *dialog,
                                             const ls200_sip_event *event) {
  /* A fallback may only change UDP to TCP for the already pinned remote
   * socket. Authorization happened immediately before this update; do not
   * let a driver use this event to pivot host, port, or TLS. */
  if (!fallback_peer_is_pinned_tcp(dialog, &event->transport))
    return LS200_STATUS_SECURITY_ERROR;
  dialog->update.peer = event->transport;
  /* The replacement TCP INVITE is a distinct PJSIP client transaction. Its
   * first event pins a new opaque handle; stale UDP cannot bind either one. */
  fallback_reseed_invite_transactions(dialog, event->dialog_update);
  return LS200_STATUS_OK;
}

static ls200_status apply_dialog_updates(ls200_sip_dialog *dialog,
                                         const ls200_sip_event *event) {
  ls200_status status;
  if (dialog == NULL) return LS200_STATUS_OK;
  if (event->kind == LS200_SIP_EVENT_TRANSPORT_FALLBACK)
    return apply_transport_fallback(dialog, event);
  if (event->dialog_update != NULL) {
    if (ls200_sip_transport_is_valid(&event->dialog_update->peer) &&
        !ls200_sip_transport_matches(&dialog->update.peer,
                                     &event->dialog_update->peer))
      return LS200_STATUS_SECURITY_ERROR;
    status = copy_update(dialog, event->dialog_update);
    if (status != LS200_STATUS_OK) return status;
  }
  if (ls200_sip_transport_is_valid(&event->transport)) {
    if (!ls200_sip_transport_matches(&dialog->update.peer, &event->transport))
      return LS200_STATUS_SECURITY_ERROR;
    dialog->update.peer = event->transport;
  }
  return LS200_STATUS_OK;
}

static int is_first_accepted_replay(const ls200_sip_dialog *dialog,
                                    const ls200_sip_event *event) {
  return event->kind == LS200_SIP_EVENT_FINAL_RESPONSE &&
         event->transaction.method == LS200_SIP_METHOD_INVITE &&
         event->response.status_code >= 200U &&
         event->response.status_code < 300U &&
         dialog->accepted_invite_transaction.handle != NULL;
}

static void observe_transactions(ls200_sip_dialog *dialog,
                                 const ls200_sip_event *event) {
  if (dialog == NULL) return;
  if (event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
      event->transaction.method == LS200_SIP_METHOD_INVITE &&
      event->transaction.cseq_number ==
          dialog->initial_invite_transaction.cseq_number &&
      dialog->initial_invite_transaction.handle == NULL)
    dialog->initial_invite_transaction = event->transaction;
  if (event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
      dialog->active_client_transaction.handle == NULL &&
      !is_first_accepted_replay(dialog, event))
    dialog->active_client_transaction = event->transaction;
}

static ls200_sip_final_classification classify_final(uint16_t status_code) {
  if (status_code < 300U || status_code > 699U) return LS200_SIP_FINAL_NONE;
  if (status_code == 408U || status_code == 429U || status_code == 480U ||
      status_code == 486U ||
      (status_code >= 500U && status_code <= 504U))
    return LS200_SIP_FINAL_TRANSIENT;
  return LS200_SIP_FINAL_PERMANENT;
}

static int is_invite_success(const ls200_sip_event *event) {
  return event->transaction.role == LS200_SIP_TRANSACTION_ROLE_CLIENT &&
         event->transaction.method == LS200_SIP_METHOD_INVITE &&
         event->response.status_code >= 200U &&
         event->response.status_code < 300U;
}

static ls200_status process_final(ls200_sip_adapter *adapter,
                                  ls200_sip_dialog *dialog,
                                  const ls200_sip_event *event,
                                  ls200_sip_event *delivered,
                                  int *suppress) {
  ls200_status status;
  if (event->kind != LS200_SIP_EVENT_FINAL_RESPONSE) return LS200_STATUS_OK;
  delivered->response.final_classification =
      classify_final(event->response.status_code);
  if (dialog == NULL) return LS200_STATUS_OK;
  if (transaction_matches(&dialog->last_final, &event->transaction) &&
      dialog->last_final_status == event->response.status_code)
    delivered->is_duplicate = 1;
  else {
    dialog->last_final = event->transaction;
    dialog->last_final_status = event->response.status_code;
  }
  if (is_invite_success(event)) {
    if (dialog->accepted_invite_transaction.handle != NULL)
      delivered->is_duplicate = 1;
    else
      dialog->accepted_invite_transaction = event->transaction;
    status = ls200_sip_driver_send_ack(adapter->driver, dialog->driver_dialog,
                                       &event->transaction);
    if (status != LS200_STATUS_OK) return status;
  }
  *suppress = delivered->is_duplicate != 0;
  return LS200_STATUS_OK;
}

static void track_provisional(ls200_sip_dialog *dialog,
                              const ls200_sip_event *event,
                              ls200_sip_event *delivered) {
  if (event->kind != LS200_SIP_EVENT_PROVISIONAL || dialog == NULL) return;
  if (transaction_matches(&dialog->last_provisional, &event->transaction))
    delivered->is_duplicate = 1;
  else
    dialog->last_provisional = event->transaction;
}

static void track_inbound(ls200_sip_dialog *dialog,
                          const ls200_sip_event *event) {
  if (dialog == NULL) return;
  if (event->kind == LS200_SIP_EVENT_REINVITE) {
    dialog->inbound_reinvite = event->transaction;
    dialog->inbound_reinvite_pending = 1;
  } else if (event->kind == LS200_SIP_EVENT_REMOTE_ACK) {
    dialog->inbound_reinvite_pending = 0;
  }
}

static int ingest_arguments_valid(const ls200_sip_adapter *adapter,
                                  const ls200_sip_dialog *dialog,
                                  const ls200_sip_event *event) {
  return adapter != NULL && event != NULL &&
         event->kind >= LS200_SIP_EVENT_PROVISIONAL &&
         event->kind <= LS200_SIP_EVENT_SESSION_TERMINATED &&
         (event_is_adapter_wide(event->kind) ||
          (dialog != NULL && dialog->driver_dialog != NULL)) &&
         (dialog == NULL || dialog->adapter == adapter);
}

ls200_status ls200_sip_adapter_ingest_driver_event(
    ls200_sip_adapter *adapter, ls200_sip_dialog *dialog,
    const ls200_sip_event *event) {
  ls200_sip_event delivered;
  ls200_status status;
  int suppress = 0;
  if (!ingest_arguments_valid(adapter, dialog, event))
    return LS200_STATUS_INVALID_ARGUMENT;
  status = validate_dialog_event(dialog, event);
  if (status == LS200_STATUS_OK) status = authorize_event(adapter, dialog, event);
  if (status == LS200_STATUS_OK) status = apply_dialog_updates(dialog, event);
  if (status != LS200_STATUS_OK) return status;
  delivered = *event;
  observe_transactions(dialog, event);
  track_provisional(dialog, event, &delivered);
  status = process_final(adapter, dialog, event, &delivered, &suppress);
  if (status != LS200_STATUS_OK || suppress) return status;
  track_inbound(dialog, event);
  if (adapter->event_callback != NULL)
    adapter->event_callback(adapter->event_context, &delivered);
  return LS200_STATUS_OK;
}
