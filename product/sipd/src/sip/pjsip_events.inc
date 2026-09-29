static int pjsip_copy_challenge_fields(aula_pjsip_dialog *dialog,
                                       const pjsip_digest_challenge *digest) {
  if (!pjsip_copy_text(dialog->realm, sizeof(dialog->realm), digest->realm) ||
      !pjsip_copy_text(dialog->nonce, sizeof(dialog->nonce), digest->nonce) ||
      !pjsip_copy_text(dialog->algorithm, sizeof(dialog->algorithm), digest->algorithm) ||
      !pjsip_copy_text(dialog->qop, sizeof(dialog->qop), digest->qop)) return 0;
  if (strcmp(dialog->algorithm, "SHA-256") != 0 || strcmp(dialog->qop, "auth") != 0) return 0;
  if (digest->opaque.slen != 0 && !pjsip_copy_text(dialog->opaque, sizeof(dialog->opaque), digest->opaque)) return 0;
  if (digest->opaque.slen == 0) dialog->opaque[0] = '\0';
  return 1;
}

static void pjsip_publish_challenge(aula_pjsip_dialog *dialog, int proxy) {
  dialog->challenge.realm = dialog->realm;
  dialog->challenge.nonce = dialog->nonce;
  dialog->challenge.opaque = dialog->opaque[0] == '\0' ? NULL : dialog->opaque;
  dialog->challenge.algorithm = dialog->algorithm;
  dialog->challenge.qop = dialog->qop;
  dialog->challenge.proxy_challenge = proxy;
}

static int pjsip_fill_challenge(aula_pjsip_dialog *dialog, const pjsip_rx_data *rdata) {
  pjsip_hdr_e header_type;
  pjsip_www_authenticate_hdr *header;
  const pjsip_digest_challenge *digest;
  int proxy;
  if (dialog == NULL || rdata == NULL || rdata->msg_info.msg == NULL) return 0;
  proxy = rdata->msg_info.msg->line.status.code == 407;
  header_type = proxy ? PJSIP_H_PROXY_AUTHENTICATE : PJSIP_H_WWW_AUTHENTICATE;
  header = (pjsip_www_authenticate_hdr *)pjsip_msg_find_hdr(rdata->msg_info.msg, header_type, NULL);
  if (header == NULL || pj_stricmp2(&header->scheme, "Digest") != 0) return 0;
  digest = &header->challenge.digest;
  if (!pjsip_copy_challenge_fields(dialog, digest)) return 0;
  pjsip_publish_challenge(dialog, proxy);
  return 1;
}

static void pjsip_discard_challenge(aula_pjsip_dialog *dialog) {
  (void)pjsip_rx_data_free_cloned(dialog->pending_challenge);
  dialog->pending_challenge = NULL;
  (void)pjsip_tx_data_dec_ref(dialog->last_request);
  dialog->last_request = NULL;
}

static pj_bool_t pjsip_handle_authentication_response(aula_pjsip_instance *instance, aula_pjsip_dialog *dialog, pjsip_rx_data *rdata) {
  pjsip_transaction *transaction = pjsip_rdata_get_tsx(rdata);
  aula_sip_event event;
  if (dialog->pending_challenge != NULL) return PJ_TRUE;
  if (transaction == NULL || transaction->last_tx == NULL || rdata->msg_info.len > (int)AULA_SIPD_MAX_SIP_MESSAGE_BYTES || !pjsip_fill_challenge(dialog, rdata) || pjsip_rx_data_clone(rdata, 0U, &dialog->pending_challenge) != PJ_SUCCESS) return PJ_FALSE;
  pjsip_tx_data_add_ref(transaction->last_tx);
  if (dialog->last_request != NULL) (void)pjsip_tx_data_dec_ref(dialog->last_request);
  dialog->last_request = transaction->last_tx;
  (void)memset(&event, 0, sizeof(event));
  event.kind = AULA_SIP_EVENT_DIGEST_CHALLENGE;
  pjsip_fill_transaction(&event.transaction, transaction, AULA_SIP_TRANSACTION_ROLE_CLIENT, pjsip_response_method(dialog, rdata->msg_info.cseq), rdata->msg_info.cseq);
  event.response.status_code = (uint16_t)rdata->msg_info.msg->line.status.code;
  event.response.cseq_number = event.transaction.cseq_number;
  event.response.cseq_method = event.transaction.method;
  event.digest_challenge = &dialog->challenge;
  if (pjsip_transport_from_rdata(rdata, &event.transport) != AULA_STATUS_OK || pjsip_emit_event(instance, dialog, &event) != AULA_STATUS_OK) pjsip_discard_challenge(dialog);
  return PJ_TRUE;
}

static pj_bool_t pjsip_emit_response_event(aula_pjsip_instance *instance, aula_pjsip_dialog *dialog, pjsip_rx_data *rdata) {
  aula_sip_event event;
  (void)memset(&event, 0, sizeof(event));
  event.kind = rdata->msg_info.msg->line.status.code < 200 ? AULA_SIP_EVENT_PROVISIONAL : AULA_SIP_EVENT_FINAL_RESPONSE;
  pjsip_fill_transaction(&event.transaction, pjsip_rdata_get_tsx(rdata), AULA_SIP_TRANSACTION_ROLE_CLIENT, pjsip_response_method(dialog, rdata->msg_info.cseq), rdata->msg_info.cseq);
  if (event.transaction.handle == NULL) return PJ_FALSE;
  event.response.status_code = (uint16_t)rdata->msg_info.msg->line.status.code;
  event.response.cseq_number = event.transaction.cseq_number;
  event.response.cseq_method = event.transaction.method;
  if (rdata->msg_info.msg->body != NULL) {
    aula_bytes sdp;
    if (pjsip_copy_sdp(dialog, rdata->msg_info.msg->body, &sdp) != AULA_STATUS_OK) return PJ_FALSE;
    event.response.has_sdp = 1;
    event.response.session_description = sdp;
  }
  if (pjsip_transport_from_rdata(rdata, &event.transport) != AULA_STATUS_OK) return PJ_FALSE;
  (void)pjsip_emit_event(instance, dialog, &event);
  return PJ_FALSE;
}

static pj_bool_t pjsip_on_rx_response(pjsip_rx_data *rdata) {
  aula_pjsip_instance *instance = pjsip_callback_instance;
  aula_pjsip_dialog *dialog;
  pjsip_dialog *native_dialog;
  int code;
  if (instance == NULL || rdata == NULL) return PJ_FALSE;
  native_dialog = pjsip_rdata_get_dlg(rdata);
  if (native_dialog == NULL) return PJ_FALSE;
  dialog = (aula_pjsip_dialog *)native_dialog->mod_data[instance->callback_module.id];
  if (dialog == NULL || rdata->msg_info.cseq == NULL || rdata->msg_info.msg == NULL) return PJ_FALSE;
  /* Session-timer refreshes are owned by invite usage and must not mutate the
   * portable transaction/CSeq state machine. */
  if (!pjsip_response_is_portable(dialog, rdata->msg_info.cseq))
    return PJ_FALSE;
  code = rdata->msg_info.msg->line.status.code;
  if ((code == 401 || code == 407) && pjsip_handle_authentication_response(instance, dialog, rdata) == PJ_TRUE) return PJ_TRUE;
  return pjsip_emit_response_event(instance, dialog, rdata);
}

static int pjsip_is_terminal_client_transport_event(const pjsip_transaction *transaction, const pjsip_event *event) {
  return transaction->role == PJSIP_ROLE_UAC && event->type == PJSIP_EVENT_TSX_STATE && transaction->state == PJSIP_TSX_STATE_TERMINATED && (event->body.tsx_state.type == PJSIP_EVENT_TRANSPORT_ERROR || event->body.tsx_state.type == PJSIP_EVENT_TIMER);
}

static aula_sip_event_kind pjsip_terminal_event_kind(const pjsip_event *event) {
  return event->body.tsx_state.type == PJSIP_EVENT_TIMER ? AULA_SIP_EVENT_TRANSACTION_TIMEOUT : AULA_SIP_EVENT_TRANSPORT_ERROR;
}

static void pjsip_emit_terminal_event(aula_pjsip_instance *instance, aula_pjsip_dialog *dialog, pjsip_transaction *transaction, aula_sip_event_kind kind) {
  aula_sip_event terminal;
  (void)memset(&terminal, 0, sizeof(terminal));
  terminal.kind = kind;
  pjsip_fill_transaction(&terminal.transaction, transaction, AULA_SIP_TRANSACTION_ROLE_CLIENT, transaction->method.id == PJSIP_INVITE_METHOD ? dialog->last_request_method : pjsip_method_from_native(&transaction->method), NULL);
  terminal.transaction.cseq_number = transaction->cseq <= 0 ? 0U : (uint32_t)transaction->cseq;
  terminal.transport = dialog->peer;
  terminal.reason_code = kind == AULA_SIP_EVENT_TRANSACTION_TIMEOUT ? "transaction_timeout" : "transport_failure";
  (void)pjsip_emit_event(instance, dialog, &terminal);
}

static void pjsip_on_tsx_state(pjsip_transaction *transaction, pjsip_event *event) {
  aula_pjsip_instance *instance = pjsip_callback_instance;
  pjsip_dialog *native_dialog;
  aula_pjsip_dialog *dialog;
  pjsip_rx_data *response;
  aula_sip_event_kind terminal_kind;
  if (instance == NULL || transaction == NULL || event == NULL) return;
  if (transaction->role != PJSIP_ROLE_UAC || event->type != PJSIP_EVENT_TSX_STATE) return;
  if (event->body.tsx_state.type == PJSIP_EVENT_RX_MSG) {
    response = event->body.tsx_state.src.rdata;
    if (response != NULL) (void)pjsip_on_rx_response(response);
    return;
  }
  if (!pjsip_is_terminal_client_transport_event(transaction, event)) return;
  native_dialog = pjsip_tsx_get_dlg(transaction);
  if (native_dialog == NULL) return;
  dialog = (aula_pjsip_dialog *)native_dialog->mod_data[instance->callback_module.id];
  if (dialog == NULL) return;
  if (!pjsip_transaction_is_portable(dialog, transaction)) return;
  if (pjsip_schedule_tcp_fallback(instance, dialog, transaction) != 0) return;
  terminal_kind = pjsip_terminal_event_kind(event);
  pjsip_emit_terminal_event(instance, dialog, transaction, terminal_kind);
}
