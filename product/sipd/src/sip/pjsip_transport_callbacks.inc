#include "media_control.h"

static void *pjsip_server_transaction_handle(pjsip_transaction *transaction,
                                             ls200_pjsip_dialog *dialog) {
  return transaction == NULL ? (void *)dialog : (void *)transaction;
}

static int pjsip_tx_destination_matches_dialog(
    const pjsip_tx_data *request, const ls200_pjsip_dialog *dialog);

static int pjsip_rx_source_matches_dialog(const pjsip_rx_data *rdata,
                                          const ls200_pjsip_dialog *dialog) {
  ls200_sip_transport_info source;
  return dialog != NULL && pjsip_transport_from_rdata(rdata, &source) ==
                               LS200_STATUS_OK &&
         source.transport == dialog->peer.transport &&
         source.remote_address_length == dialog->peer.remote_address_length &&
         source.remote_port == dialog->peer.remote_port &&
         memcmp(source.remote_address, dialog->peer.remote_address,
                source.remote_address_length) == 0;
}

/* Registered before the transaction layer. Returning true consumes an
 * untrusted matched-dialog message before transaction or invite/session code
 * can produce a response or update native state. */
static ls200_pjsip_dialog *pjsip_guard_find_dialog(
    ls200_pjsip_instance *instance, const pjsip_msg *message) {
  const pjsip_cid_hdr *call_id;
  const pjsip_fromto_hdr *local;
  ls200_pjsip_dialog *dialog;
  if (instance == NULL || message == NULL) return NULL;
  call_id = (const pjsip_cid_hdr *)pjsip_msg_find_hdr(message, PJSIP_H_CALL_ID, NULL);
  local = (const pjsip_fromto_hdr *)pjsip_msg_find_hdr(
      message, message->type == PJSIP_REQUEST_MSG ? PJSIP_H_TO : PJSIP_H_FROM, NULL);
  if (call_id == NULL || local == NULL || call_id->id.slen <= 0 || local->tag.slen <= 0)
    return NULL;
  for (dialog = instance->guard_head; dialog != NULL; dialog = dialog->guard_next) {
    if ((pj_ssize_t)strlen(dialog->guard_call_id) == call_id->id.slen &&
        (pj_ssize_t)strlen(dialog->guard_local_tag) == local->tag.slen &&
        memcmp(dialog->guard_call_id, call_id->id.ptr, (size_t)call_id->id.slen) == 0 &&
        memcmp(dialog->guard_local_tag, local->tag.ptr, (size_t)local->tag.slen) == 0)
      return dialog;
  }
  return NULL;
}

static ls200_status pjsip_guard_track_dialog(ls200_pjsip_instance *instance,
                                             ls200_pjsip_dialog *dialog) {
  const pj_str_t *call_id;
  const pj_str_t *local_tag;
  if (instance == NULL || dialog == NULL || dialog->dialog == NULL ||
      dialog->guard_tracked != 0)
    return LS200_STATUS_INVALID_ARGUMENT;
  call_id = &dialog->dialog->call_id->id;
  local_tag = &dialog->dialog->local.info->tag;
  if (call_id->slen <= 0 || local_tag->slen <= 0 ||
      (size_t)call_id->slen >= sizeof(dialog->guard_call_id) ||
      (size_t)local_tag->slen >= sizeof(dialog->guard_local_tag))
    return LS200_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(dialog->guard_call_id, call_id->ptr, (size_t)call_id->slen);
  dialog->guard_call_id[call_id->slen] = '\0';
  (void)memcpy(dialog->guard_local_tag, local_tag->ptr, (size_t)local_tag->slen);
  dialog->guard_local_tag[local_tag->slen] = '\0';
  dialog->guard_next = instance->guard_head;
  instance->guard_head = dialog;
  dialog->guard_tracked = 1;
  return LS200_STATUS_OK;
}

static void pjsip_guard_untrack_dialog(ls200_pjsip_instance *instance,
                                       ls200_pjsip_dialog *dialog) {
  ls200_pjsip_dialog **cursor;
  if (instance == NULL || dialog == NULL || dialog->guard_tracked == 0) return;
  for (cursor = &instance->guard_head; *cursor != NULL;
       cursor = &(*cursor)->guard_next) {
    if (*cursor == dialog) {
      *cursor = dialog->guard_next;
      break;
    }
  }
  dialog->guard_next = NULL;
  dialog->guard_tracked = 0;
  (void)memset(dialog->guard_call_id, 0, sizeof(dialog->guard_call_id));
  (void)memset(dialog->guard_local_tag, 0, sizeof(dialog->guard_local_tag));
}

static pj_bool_t pjsip_on_rx_guard(pjsip_rx_data *rdata) {
  ls200_pjsip_instance *instance = pjsip_callback_instance;
  ls200_pjsip_dialog *dialog;
  if (instance == NULL || rdata == NULL || rdata->msg_info.msg == NULL)
    return PJ_FALSE;
  dialog = pjsip_guard_find_dialog(instance, rdata->msg_info.msg);
  return dialog != NULL && !pjsip_rx_source_matches_dialog(rdata, dialog) ?
             PJ_TRUE : PJ_FALSE;
}

static pj_bool_t pjsip_handle_remote_termination(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_transaction *transaction, pjsip_rx_data *rdata,
    pjsip_method_e method) {
  ls200_sip_event event;
  (void)memset(&event, 0, sizeof(event));
  event.kind = method == PJSIP_BYE_METHOD ? LS200_SIP_EVENT_REMOTE_BYE :
                                            LS200_SIP_EVENT_REMOTE_CANCEL;
  pjsip_fill_transaction(&event.transaction,
                         pjsip_server_transaction_handle(transaction, dialog),
                         LS200_SIP_TRANSACTION_ROLE_SERVER,
                         pjsip_method_from_native(&rdata->msg_info.cseq->method),
                         rdata->msg_info.cseq);
  if (pjsip_transport_from_rdata(rdata, &event.transport) != LS200_STATUS_OK)
    return PJ_FALSE;
  return pjsip_create_queued_response(instance, dialog, transaction, rdata, 200,
                                      &event) == LS200_STATUS_OK ? PJ_TRUE :
                                                                  PJ_FALSE;
}

static pj_bool_t pjsip_handle_remote_ack(ls200_pjsip_instance *instance,
                                         ls200_pjsip_dialog *dialog,
                                         pjsip_transaction *transaction,
                                         pjsip_rx_data *rdata) {
  ls200_sip_event event;
  (void)memset(&event, 0, sizeof(event));
  event.kind = LS200_SIP_EVENT_REMOTE_ACK;
  pjsip_fill_transaction(&event.transaction,
                         pjsip_server_transaction_handle(transaction, dialog),
                         LS200_SIP_TRANSACTION_ROLE_SERVER,
                         LS200_SIP_METHOD_ACK, rdata->msg_info.cseq);
  if (pjsip_transport_from_rdata(rdata, &event.transport) != LS200_STATUS_OK)
    return PJ_FALSE;
  (void)pjsip_emit_event(instance, dialog, &event);
  return PJ_TRUE;
}

static int pjsip_is_picture_fast_update(const pjsip_msg *message) {
  const pjsip_msg_body *body;
  if (message == NULL || message->line.req.method.id != PJSIP_OTHER_METHOD ||
      pj_stricmp2(&message->line.req.method.name, "INFO") != 0)
    return 0;
  body = message->body;
  return body != NULL && body->data != NULL && body->len != 0U &&
      body->len <= LS200_SIPD_MAX_SIP_MESSAGE_BYTES &&
      pj_stricmp2(&body->content_type.type, "application") == 0 &&
      pj_stricmp2(&body->content_type.subtype, "media_control+xml") == 0 &&
      ls200_sip_media_control_is_picture_update(
          (ls200_bytes){(const uint8_t *)body->data, (size_t)body->len});
}

static pj_bool_t pjsip_handle_picture_fast_update(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_transaction *transaction, pjsip_rx_data *rdata) {
  ls200_sip_adapter *adapter;
  ls200_status status;
  if (instance == NULL || dialog == NULL || transaction == NULL ||
      rdata == NULL || dialog->pending_response != NULL ||
      instance->sink != ls200_sip_adapter_driver_sink)
    return PJ_FALSE;
  adapter = (ls200_sip_adapter *)instance->sink_context;
  status = pjsip_create_queued_response(instance, dialog, transaction, rdata,
                                        200, NULL);
  if (status != LS200_STATUS_OK) return PJ_FALSE;
  status = ls200_sip_adapter_note_picture_fast_update(adapter, dialog);
  if (status != LS200_STATUS_OK) {
    pjsip_remove_pending_response(instance, dialog);
    pjsip_clear_pending_response(dialog);
    return PJ_FALSE;
  }
  return PJ_TRUE;
}

static ls200_status pjsip_create_pending_invite_response(
    ls200_pjsip_dialog *dialog, int status_code,
    const pjmedia_sdp_session *local_sdp) {
  pj_status_t status;
  if (dialog == NULL || dialog->invite == NULL ||
      dialog->pending_invite_request == NULL || dialog->pending_response != NULL)
    return LS200_STATUS_STATE_ERROR;
  status = pjsip_inv_initial_answer(dialog->invite, dialog->pending_invite_request,
                                    status_code, NULL, local_sdp,
                                    &dialog->pending_response);
  (void)pjsip_rx_data_free_cloned(dialog->pending_invite_request);
  dialog->pending_invite_request = NULL;
  if (status != PJ_SUCCESS) return pjsip_status(status);
  dialog->pending_response_is_invite = 1;
  return LS200_STATUS_OK;
}

static pj_bool_t pjsip_reject_invalid_reinvite(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_transaction *transaction, pjsip_rx_data *rdata,
    pjsip_inv_session *invite) {
  if (invite != NULL) {
    if (pjsip_rx_data_clone(rdata, 0U, &dialog->pending_invite_request) ==
            PJ_SUCCESS &&
        pjsip_create_pending_invite_response(dialog, 488, NULL) ==
            LS200_STATUS_OK) {
      dialog->pending_request_transaction = transaction;
      return pjsip_queue_pending_response(instance, dialog) == LS200_STATUS_OK
                 ? PJ_TRUE
                 : PJ_FALSE;
    }
    pjsip_clear_pending_response(dialog);
    (void)pjsip_endpt_respond_stateless(instance->endpoint, rdata, 488, NULL,
                                        NULL, NULL);
    return PJ_TRUE;
  }
  return pjsip_create_queued_response(instance, dialog, transaction, rdata,
                                      488, NULL) == LS200_STATUS_OK
             ? PJ_TRUE
             : PJ_FALSE;
}

static int pjsip_prepare_reinvite_response(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_transaction *transaction, pjsip_rx_data *rdata,
    pjsip_inv_session *invite) {
  if (invite == NULL &&
      pjsip_dlg_create_response(dialog->dialog, rdata, 200, NULL,
                                &dialog->pending_response) != PJ_SUCCESS) {
    pjsip_clear_pending_response(dialog);
    (void)pjsip_endpt_respond_stateless(instance->endpoint, rdata, 500, NULL,
                                        NULL, NULL);
    return 0;
  }
  dialog->pending_request_transaction = transaction;
  if (invite != NULL &&
      pjsip_rx_data_clone(rdata, 0U, &dialog->pending_invite_request) !=
          PJ_SUCCESS) {
    pjsip_clear_pending_response(dialog);
    (void)pjsip_endpt_respond_stateless(instance->endpoint, rdata, 500, NULL,
                                        NULL, NULL);
    return 0;
  }
  return 1;
}

static void pjsip_reinvite_event_failed(ls200_pjsip_instance *instance,
                                        ls200_pjsip_dialog *dialog,
                                        pjsip_rx_data *rdata,
                                        pjsip_inv_session *invite) {
  if (invite != NULL) {
    if (pjsip_create_pending_invite_response(dialog, 500, NULL) ==
            LS200_STATUS_OK &&
        pjsip_queue_pending_response(instance, dialog) == LS200_STATUS_OK)
      return;
    pjsip_clear_pending_response(dialog);
    (void)pjsip_endpt_respond_stateless(instance->endpoint, rdata, 500, NULL,
                                        NULL, NULL);
  } else if (pjsip_replace_and_queue_pending_response(instance, dialog, 500) !=
             LS200_STATUS_OK) {
    pjsip_clear_pending_response(dialog);
  }
}

static pj_bool_t pjsip_handle_reinvite(ls200_pjsip_instance *instance,
                                       ls200_pjsip_dialog *dialog,
                                       pjsip_transaction *transaction,
                                       pjsip_rx_data *rdata,
                                       pjsip_inv_session *invite) {
  ls200_sip_event event;
  ls200_bytes offer;
  ls200_status status;
  if (transaction == NULL || dialog->pending_response != NULL ||
      dialog->pending_invite_request != NULL) {
    (void)pjsip_endpt_respond_stateless(instance->endpoint, rdata, 491, NULL,
                                        NULL, NULL);
    return PJ_TRUE;
  }
  status = pjsip_copy_sdp(dialog, rdata->msg_info.msg->body, &offer);
  if (status != LS200_STATUS_OK)
    return pjsip_reject_invalid_reinvite(instance, dialog, transaction, rdata,
                                         invite);
  if (!pjsip_prepare_reinvite_response(instance, dialog, transaction, rdata,
                                       invite))
    return PJ_TRUE;
  (void)memset(&event, 0, sizeof(event));
  event.kind = LS200_SIP_EVENT_REINVITE;
  pjsip_fill_transaction(&event.transaction, transaction,
                         LS200_SIP_TRANSACTION_ROLE_SERVER,
                         LS200_SIP_METHOD_REINVITE, rdata->msg_info.cseq);
  event.reinvite = &(ls200_sip_reinvite){offer, 1, event.transaction};
  if (pjsip_transport_from_rdata(rdata, &event.transport) != LS200_STATUS_OK ||
      pjsip_emit_event(instance, dialog, &event) != LS200_STATUS_OK)
    pjsip_reinvite_event_failed(instance, dialog, rdata, invite);
  return PJ_TRUE;
}

/* Direct reliable re-INVITEs are consumed by invite usage before the generic
 * dialog module. Preserve that session's SDP negotiator by routing the
 * authorized offer through the portable media event and answering later with
 * pjsip_inv_answer()/pjsip_inv_send_msg(). */
static pj_status_t pjsip_inv_reinvite_received(
    pjsip_inv_session *invite, const pjmedia_sdp_session *offer,
    pjsip_rx_data *rdata) {
  ls200_pjsip_instance *instance = pjsip_callback_instance;
  ls200_pjsip_dialog *dialog;
  pjsip_transaction *transaction;
  (void)offer;
  if (instance == NULL || invite == NULL || rdata == NULL || invite->dlg == NULL ||
      instance->callback_module.id < 0)
    return PJ_EINVAL;
  dialog = (ls200_pjsip_dialog *)invite->dlg->mod_data[instance->callback_module.id];
  if (dialog == NULL || !pjsip_rx_source_matches_dialog(rdata, dialog))
    return PJ_EINVAL;
  transaction = pjsip_rdata_get_tsx(rdata);
  return pjsip_handle_reinvite(instance, dialog, transaction, rdata, invite) ?
             PJ_SUCCESS : PJ_EUNKNOWN;
}

static pj_bool_t pjsip_on_rx_request(pjsip_rx_data *rdata) {
  ls200_pjsip_instance *instance = pjsip_callback_instance;
  ls200_pjsip_dialog *dialog;
  pjsip_dialog *native_dialog;
  pjsip_transaction *transaction;
  pjsip_method_e method;
  if (instance == NULL || rdata == NULL || rdata->msg_info.msg == NULL ||
      rdata->msg_info.cseq == NULL) return PJ_FALSE;
  native_dialog = pjsip_rdata_get_dlg(rdata);
  if (native_dialog == NULL) return PJ_FALSE;
  dialog = (ls200_pjsip_dialog *)native_dialog->mod_data[instance->callback_module.id];
  if (dialog == NULL) return PJ_FALSE;
  /* A matched SIP dialog is not enough: a forged request may reuse its
   * headers. Reject before creating a response, emitting an event, or handing
   * the request to invite usage. */
  if (!pjsip_rx_source_matches_dialog(rdata, dialog)) return PJ_FALSE;
  transaction = pjsip_rdata_get_tsx(rdata);
  method = rdata->msg_info.msg->line.req.method.id;
  pjsip_observe_in_dialog_info(dialog, rdata);
  if (method == PJSIP_BYE_METHOD || method == PJSIP_CANCEL_METHOD)
    return pjsip_handle_remote_termination(instance, dialog, transaction, rdata, method);
  if (method == PJSIP_ACK_METHOD)
    return pjsip_handle_remote_ack(instance, dialog, transaction, rdata);
  if (pjsip_is_picture_fast_update(rdata->msg_info.msg))
    return pjsip_handle_picture_fast_update(instance, dialog, transaction,
                                            rdata);
  return method == PJSIP_INVITE_METHOD ?
             pjsip_handle_reinvite(instance, dialog, transaction, rdata, NULL) : PJ_FALSE;
}

static pj_status_t pjsip_on_tx_request(pjsip_tx_data *request) {
  ls200_pjsip_instance *instance = pjsip_callback_instance;
  pjsip_dialog *native_dialog;
  ls200_pjsip_dialog *dialog;
  if (instance == NULL || request == NULL || instance->callback_module.id < 0)
    return PJ_SUCCESS;
  native_dialog = pjsip_tdata_get_dlg(request);
  if (native_dialog == NULL) return PJ_SUCCESS;
  dialog = (ls200_pjsip_dialog *)native_dialog->mod_data[instance->callback_module.id];
  if (dialog == NULL) return PJ_SUCCESS;
  /* PJSIP has selected the actual transport and sockaddr at this point. */
  if (dialog->closing == 0 &&
      pjsip_tx_destination_matches_dialog(request, dialog))
    return PJ_SUCCESS;
  /* This final gate also covers requests created internally by the session
   * timer. Retire the call after PJSIP unwinds instead of hiding a protected
   * refresh failure from the portable endpoint. */
  if (dialog->closing == 0)
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_FORBIDDEN);
  return PJ_EINVALIDOP;
}

static void pjsip_inv_state_changed(pjsip_inv_session *invite, pjsip_event *event) {
  ls200_pjsip_instance *instance = pjsip_callback_instance;
  ls200_pjsip_dialog *dialog;
  (void)event;
  if (instance == NULL || invite == NULL || invite->dlg == NULL ||
      invite->state != PJSIP_INV_STATE_DISCONNECTED ||
      instance->callback_module.id < 0)
    return;
  dialog = (ls200_pjsip_dialog *)
      invite->dlg->mod_data[instance->callback_module.id];
  if (dialog != NULL)
    pjsip_log_invite_disconnected((int)invite->state, (int)invite->cause,
                                  dialog->closing != 0);
  /* Local teardown marks closing first. Only an independently initiated
   * native disconnect crosses the portable boundary. */
  if (dialog == NULL || dialog->closing != 0 || dialog->invite != invite)
    return;
  dialog->invite = NULL;
  dialog->closing = 1;
  pjsip_queue_terminal_event(instance, dialog, (int)invite->cause);
}

static pjsip_redirect_op pjsip_inv_stop_redirect(pjsip_inv_session *invite,
                                                 const pjsip_uri *target,
                                                 const pjsip_event *event) {
  (void)invite;
  (void)target;
  (void)event;
  /* The low-level adapter has no redirect-following policy. */
  return PJSIP_REDIRECT_STOP;
}

static ls200_status pjsip_authorize_dialog_destinations(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_sip_transport_info *out_next_hop);
static ls200_status pjsip_authorize_initial_dialog_destinations(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_sip_transport_info *out_next_hop);
static ls200_status pjsip_pin_dialog_request(pjsip_tx_data *request,
                                             const ls200_sip_transport_info *peer);

static ls200_status pjsip_send_dialog_request(ls200_pjsip_instance *instance,
                                               ls200_pjsip_dialog *dialog,
                                               const pjsip_method *method,
                                               ls200_sip_method request_method,
                                               int cseq, ls200_bytes sdp) {
  pjsip_tx_data *request = NULL;
  ls200_sip_transport_info next_hop;
  pj_status_t status;
  pj_int32_t saved_native_cseq;
  if (instance == NULL || dialog == NULL || dialog->dialog == NULL || method == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (pjsip_authorize_dialog_destinations(instance, dialog, &next_hop) !=
      LS200_STATUS_OK)
    return LS200_STATUS_PERMISSION_DENIED;
  saved_native_cseq = dialog->dialog->local.cseq;
  status = pjsip_dlg_create_request(dialog->dialog, method, cseq, &request);
  if (status != PJ_SUCCESS) {
    dialog->dialog->local.cseq = saved_native_cseq;
    return pjsip_status(status);
  }
  if (pjsip_pin_dialog_request(request, &next_hop) != LS200_STATUS_OK) {
    (void)pjsip_tx_data_dec_ref(request);
    dialog->dialog->local.cseq = saved_native_cseq;
    return LS200_STATUS_PERMISSION_DENIED;
  }
  if (sdp.data != NULL) {
    ls200_status body_status = pjsip_attach_sdp(request, sdp);
    if (body_status != LS200_STATUS_OK) {
      (void)pjsip_tx_data_dec_ref(request);
      dialog->dialog->local.cseq = saved_native_cseq;
      return body_status;
    }
  }
  if (!pjsip_track_portable_request(dialog, request, request_method)) {
    (void)pjsip_tx_data_dec_ref(request);
    dialog->dialog->local.cseq = saved_native_cseq;
    return LS200_STATUS_STATE_ERROR;
  }
  pjsip_tx_data_add_ref(request);
  if (dialog->last_request != NULL) (void)pjsip_tx_data_dec_ref(dialog->last_request);
  dialog->last_request = request;
  dialog->last_request_method = request_method;
  status = pjsip_dlg_send_request(dialog->dialog, request, -1, NULL);
  if (status != PJ_SUCCESS) {
    (void)pjsip_tx_data_dec_ref(dialog->last_request);
    dialog->last_request = NULL;
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_GONE);
    dialog->closing = 1;
    pjsip_terminate_invite_session(dialog, PJSIP_SC_GONE);
    return LS200_STATUS_END;
  }
  return pjsip_status(status);
}

static ls200_status pjsip_set_local_uri(ls200_pjsip_instance *instance,
                                        const ls200_pjsip_context *settings,
                                        const ls200_sip_endpoint_config *endpoint) {
  const char *local_uri = settings->local_uri[0] == '\0' ? endpoint->outbound_uri :
                                                            settings->local_uri;
  if (local_uri == NULL || strlen(local_uri) >= sizeof(instance->local_uri_storage))
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(instance->local_uri_storage, local_uri, strlen(local_uri) + 1U);
  instance->local_uri = pj_str(instance->local_uri_storage);
  return LS200_STATUS_OK;
}

static ls200_status pjsip_set_auth_username(ls200_pjsip_instance *instance,
                                            const char *auth_username) {
  if (auth_username == NULL) return LS200_STATUS_OK;
  if (!ls200_sip_text_is_safe(auth_username, sizeof(instance->auth_username)))
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(instance->auth_username, auth_username, strlen(auth_username) + 1U);
  return LS200_STATUS_OK;
}

static ls200_status pjsip_instance_copy_crc_route(
    ls200_pjsip_instance *instance, const ls200_pjsip_context *settings) {
  return ls200_sip_crc_route_initialize(&instance->crc_route,
                                        settings->crc_address);
}

static ls200_status pjsip_start_instance(ls200_pjsip_instance *instance,
                                          const ls200_pjsip_context *settings,
                                          const ls200_sip_endpoint_config *endpoint) {
  ls200_status status;
  status = pjsip_initialize_stack(instance);
  if (status == LS200_STATUS_OK) status = pjsip_start_transports(instance, settings, endpoint);
  if (status == LS200_STATUS_OK) status = pjsip_register_proxy(instance, settings, endpoint);
  return status;
}

static ls200_status pjsip_create(void *context,
                                 const ls200_sip_endpoint_config *endpoint,
                                 ls200_sip_driver_event_sink sink,
                                 void *sink_context, void **out_instance) {
  ls200_pjsip_instance *instance;
  ls200_status status;
  const ls200_pjsip_context *settings = (const ls200_pjsip_context *)context;
  if (settings == NULL || endpoint == NULL || sink == NULL || out_instance == NULL ||
      *out_instance != NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (pjsip_active_instance != NULL) return LS200_STATUS_STATE_ERROR;
  instance = (ls200_pjsip_instance *)calloc(1U, sizeof(*instance));
  if (instance == NULL) return LS200_STATUS_INTERNAL_ERROR;
  instance->sink = sink;
  instance->sink_context = sink_context;
  instance->profile = endpoint->profile;
  instance->allow_tcp_fallback = endpoint->allow_tcp_fallback != 0;
  status = pjsip_instance_copy_crc_route(instance, settings);
  if (status == LS200_STATUS_OK)
    status = pjsip_set_local_uri(instance, settings, endpoint);
  if (status == LS200_STATUS_OK)
    status = pjsip_set_auth_username(instance, endpoint->auth_username);
  if (status == LS200_STATUS_OK) status = pjsip_start_instance(instance, settings, endpoint);
  if (status != LS200_STATUS_OK) {
    pjsip_release_instance(instance);
    return status;
  }
  pjsip_tsx_set_max_retransmit_count((int)endpoint->max_retransmissions);
  pjsip_active_instance = instance;
  *out_instance = instance;
  return LS200_STATUS_OK;
}
