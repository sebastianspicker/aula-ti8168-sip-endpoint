static ls200_status pjsip_send_invite_message(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_inv_session *invite, pjsip_tx_data *message,
    ls200_sip_method request_method) {
  ls200_sip_transport_info next_hop;
  pj_status_t status;
  if (instance == NULL || dialog == NULL || invite == NULL || message == NULL ||
      dialog->closing != 0)
    return LS200_STATUS_STATE_ERROR;
  if (pjsip_authorize_dialog_destinations(instance, dialog, &next_hop) !=
          LS200_STATUS_OK ||
      pjsip_pin_dialog_request(message, &next_hop) != LS200_STATUS_OK) {
    (void)pjsip_tx_data_dec_ref(message);
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_FORBIDDEN);
    return LS200_STATUS_END;
  }
  if (!pjsip_track_portable_request(dialog, message, request_method)) {
    (void)pjsip_tx_data_dec_ref(message);
    pjsip_queue_terminal_event(instance, dialog,
                               PJSIP_SC_INTERNAL_SERVER_ERROR);
    return LS200_STATUS_END;
  }
  status = pjsip_inv_send_msg(invite, message);
  if (status != PJ_SUCCESS) {
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_GONE);
    return LS200_STATUS_END;
  }
  return LS200_STATUS_OK;
}

static ls200_status pjsip_preflight_invite_operation(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog) {
  ls200_sip_transport_info next_hop;
  if (instance == NULL || dialog == NULL || dialog->invite == NULL ||
      dialog->closing != 0)
    return LS200_STATUS_STATE_ERROR;
  return pjsip_authorize_dialog_destinations(instance, dialog, &next_hop);
}

static ls200_status pjsip_parse_reinvite_sdp(ls200_pjsip_dialog *dialog,
                                             ls200_bytes local_sdp,
                                             pjmedia_sdp_session **out_sdp) {
  char *sdp_text;
  if (dialog == NULL || dialog->dialog == NULL || out_sdp == NULL ||
      local_sdp.data == NULL || local_sdp.length == 0U ||
      local_sdp.length > LS200_SIPD_MAX_SDP_BYTES)
    return LS200_STATUS_INVALID_ARGUMENT;
  *out_sdp = NULL;
  sdp_text = (char *)pj_pool_alloc(dialog->dialog->pool, local_sdp.length + 1U);
  if (sdp_text == NULL) return LS200_STATUS_INTERNAL_ERROR;
  (void)memcpy(sdp_text, local_sdp.data, local_sdp.length);
  sdp_text[local_sdp.length] = '\0';
  return pjsip_status(pjmedia_sdp_parse(dialog->dialog->pool, sdp_text,
                                        (pj_size_t)local_sdp.length, out_sdp));
}

static ls200_status pjsip_ack(void *c, void *i, void *d,
                              const ls200_sip_transaction_identity *t) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  (void)c;
  if (t == NULL || dialog == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  /* Reliable session-timer dialogs install pjsip_inv's manual ACK callback.
   * It creates and pins the ACK only after invite usage has processed the
   * 2xx; the portable adapter's synchronous acknowledgement remains a
   * completion signal and must not create a duplicate request. */
  if (dialog->invite != NULL) return LS200_STATUS_OK;
  return pjsip_send_dialog_request((ls200_pjsip_instance *)i, dialog, &pjsip_ack_method,
                                   LS200_SIP_METHOD_ACK, (int)t->cseq_number,
                                   (ls200_bytes){NULL, 0U});
}
static ls200_status pjsip_cancel(void *c, void *i, void *d, uint32_t seq) {
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  pjsip_tx_data *cancel = NULL;
  pj_status_t status;
  (void)c;
  (void)seq;
  if (instance == NULL || dialog == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (dialog->invite != NULL) {
    ls200_status preflight = pjsip_preflight_invite_operation(instance, dialog);
    if (preflight != LS200_STATUS_OK) return preflight;
    /* The portable cancel operation normally applies to the initial INVITE;
     * retain the re-INVITE branch for a confirmed usage if that API evolves. */
    status = dialog->invite->state == PJSIP_INV_STATE_CONFIRMED ?
        pjsip_inv_cancel_reinvite(dialog->invite, &cancel) :
        pjsip_inv_end_session(dialog->invite, PJSIP_SC_REQUEST_TERMINATED,
                              NULL, &cancel);
    if (status != PJ_SUCCESS) return pjsip_status(status);
    return cancel == NULL ? LS200_STATUS_OK :
           pjsip_send_invite_message(instance, dialog, dialog->invite, cancel,
                                     LS200_SIP_METHOD_CANCEL);
  }
  return pjsip_send_dialog_request((ls200_pjsip_instance *)i,
                                   dialog, &pjsip_cancel_method,
                                   LS200_SIP_METHOD_CANCEL, (int)seq,
                                   (ls200_bytes){NULL, 0U});
}
static ls200_status pjsip_bye(void *c, void *i, void *d, uint32_t seq) {
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  pjsip_tx_data *bye = NULL;
  ls200_status result;
  pj_status_t status;
  (void)c;
  (void)seq;
  if (instance == NULL || dialog == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (dialog->invite != NULL) {
    status = pjsip_preflight_invite_operation(instance, dialog);
    if (status != LS200_STATUS_OK) return status;
    status = pjsip_inv_end_session(dialog->invite, PJSIP_SC_OK, NULL, &bye);
    if (status != PJ_SUCCESS) return pjsip_status(status);
    if (bye == NULL) return LS200_STATUS_OK;
    result = pjsip_send_invite_message(instance, dialog, dialog->invite, bye,
                                       LS200_SIP_METHOD_BYE);
    /* end_session() has stopped the timer; a failed final send also retires
     * the usage so a later internal retry cannot outlive this dialog. */
    if (result != LS200_STATUS_OK)
      pjsip_terminate_invite_session(dialog, PJSIP_SC_GONE);
    return result;
  }
  return pjsip_send_dialog_request((ls200_pjsip_instance *)i,
                                   dialog, &pjsip_bye_method,
                                   LS200_SIP_METHOD_BYE, -1,
                                   (ls200_bytes){NULL, 0U});
}
static ls200_status pjsip_reinvite(void *c, void *i, void *d,
                                   const ls200_sip_reinvite_request *request) {
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  pjmedia_sdp_session *offer = NULL;
  pjsip_tx_data *reinvite = NULL;
  ls200_status status;
  (void)c;
  if (instance == NULL || dialog == NULL || request == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (dialog->invite != NULL) {
    status = pjsip_preflight_invite_operation(instance, dialog);
    if (status != LS200_STATUS_OK) return status;
    status = pjsip_parse_reinvite_sdp(dialog, request->local_sdp, &offer);
    if (status != LS200_STATUS_OK) return status;
    status = pjsip_status(pjsip_inv_reinvite(dialog->invite, NULL, offer, &reinvite));
    if (status != LS200_STATUS_OK) return status;
    return pjsip_send_invite_message(instance, dialog, dialog->invite, reinvite,
                                     LS200_SIP_METHOD_REINVITE);
  }
  return pjsip_send_dialog_request((ls200_pjsip_instance *)i,
                                   dialog, &pjsip_invite_method,
                                   LS200_SIP_METHOD_REINVITE, -1, request->local_sdp);
}

static int pjsip_reinvite_response_is_valid(
    const ls200_pjsip_instance *instance, const ls200_pjsip_dialog *dialog,
    const ls200_sip_transaction_identity *transaction) {
  if (instance == NULL || dialog == NULL || transaction == NULL) return 0;
  if ((dialog->invite == NULL && dialog->pending_response == NULL) ||
      (dialog->invite != NULL && dialog->pending_response != NULL) ||
      dialog->pending_response_queued != 0 ||
      dialog->pending_request_transaction == NULL ||
      dialog->pending_request_transaction != transaction->handle)
    return 0;
  return transaction->method == LS200_SIP_METHOD_REINVITE &&
         transaction->cseq_number ==
             (uint32_t)dialog->pending_request_transaction->cseq;
}

static ls200_status pjsip_answer(void *c, void *i, void *d, ls200_bytes s,
                                 const ls200_sip_transaction_identity *t) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  pjmedia_sdp_session *answer = NULL;
  ls200_status attach_status;
  (void)c;
  if (!pjsip_reinvite_response_is_valid(instance, dialog, t))
    return LS200_STATUS_STATE_ERROR;
  if (dialog->invite != NULL) {
    attach_status = pjsip_parse_reinvite_sdp(dialog, s, &answer);
    if (attach_status != LS200_STATUS_OK) return attach_status;
    attach_status = pjsip_create_pending_invite_response(dialog, 200, answer);
    if (attach_status != LS200_STATUS_OK) return attach_status;
    return pjsip_queue_pending_response(instance, dialog);
  }
  attach_status = pjsip_attach_sdp(dialog->pending_response, s);
  if (attach_status != LS200_STATUS_OK) return attach_status;
  return pjsip_queue_pending_response(instance, dialog);
}
static ls200_status pjsip_reject(void *c, void *i, void *d, uint16_t code,
                                 const ls200_sip_transaction_identity *t) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  pj_status_t status;
  (void)c;
  if (code < 300U || code > 699U ||
      !pjsip_reinvite_response_is_valid(instance, dialog, t))
    return LS200_STATUS_STATE_ERROR;
  if (dialog->invite != NULL) {
    if (pjsip_create_pending_invite_response(dialog, (int)code, NULL) !=
        LS200_STATUS_OK)
      return LS200_STATUS_STATE_ERROR;
    return pjsip_queue_pending_response(instance, dialog);
  }
  status = pjsip_dlg_modify_response(dialog->dialog, dialog->pending_response,
                                     (int)code, NULL);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  /* modify_response() adds a reference. Retain exactly the original owned
   * reference while the response waits in the deferred queue. */
  (void)pjsip_tx_data_dec_ref(dialog->pending_response);
  return pjsip_queue_pending_response(instance, dialog);
}

static int pjsip_digest_request_is_valid(
    const ls200_pjsip_dialog *dialog, const ls200_pjsip_instance *instance,
    const ls200_sip_digest_response *response) {
  return dialog != NULL && response != NULL && response->challenge != NULL &&
         response->response.data != NULL && response->response.length != 0U &&
         response->response.length <= LS200_SIPD_MAX_SIP_MESSAGE_BYTES &&
         dialog->pending_challenge != NULL && dialog->last_request != NULL &&
         instance != NULL && instance->auth_username[0] != '\0';
}

static pj_status_t pjsip_send_digest_retry(ls200_pjsip_instance *instance,
                                           ls200_pjsip_dialog *dialog,
                                           pjsip_tx_data *retry) {
  ls200_sip_transport_info next_hop;
  pj_status_t status;
  if (pjsip_authorize_dialog_destinations(instance, dialog, &next_hop) !=
          LS200_STATUS_OK ||
      pjsip_pin_dialog_request(retry, &next_hop) != LS200_STATUS_OK) {
    (void)pjsip_tx_data_dec_ref(retry);
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_FORBIDDEN);
    pjsip_terminate_invite_session(dialog, PJSIP_SC_FORBIDDEN);
    return PJ_EINVALIDOP;
  }
  status = pjsip_dlg_send_request(dialog->dialog, retry, -1, NULL);
  if (status != PJ_SUCCESS) {
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_GONE);
    pjsip_terminate_invite_session(dialog, PJSIP_SC_GONE);
  }
  return status;
}

static ls200_status pjsip_digest(void *c, void *i, void *d,
                                 const ls200_sip_digest_response *r) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  pjsip_tx_data *retry = NULL;
  pjsip_cred_info credential;
  pj_status_t status;
  (void)c;
  if (!pjsip_digest_request_is_valid(dialog, instance, r))
    return LS200_STATUS_STATE_ERROR;
  (void)memset(&credential, 0, sizeof(credential));
  credential.scheme = pj_str("digest");
  credential.username = pj_str(instance->auth_username);
  credential.realm = pj_str(dialog->realm);
  credential.data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
  credential.data.ptr = (char *)r->response.data;
  credential.data.slen = (pj_ssize_t)r->response.length;
  credential.algorithm_type = PJSIP_AUTH_ALGORITHM_SHA256;
  status = pjsip_auth_clt_set_credentials(&dialog->dialog->auth_sess, 1, &credential);
  if (status == PJ_SUCCESS)
    status = pjsip_auth_clt_reinit_req(&dialog->dialog->auth_sess,
                                       dialog->pending_challenge,
                                       dialog->last_request, &retry);
  if (dialog->dialog->auth_sess.cred_cnt != 0 &&
      dialog->dialog->auth_sess.cred_info[0].data.ptr != NULL)
    pjsip_wipe(dialog->dialog->auth_sess.cred_info[0].data.ptr,
               (size_t)dialog->dialog->auth_sess.cred_info[0].data.slen);
  (void)pjsip_auth_clt_set_credentials(&dialog->dialog->auth_sess, 0, &credential);
  if (status != PJ_SUCCESS) return LS200_STATUS_SECURITY_ERROR;
  status = pjsip_send_digest_retry(instance, dialog, retry);
  (void)pjsip_rx_data_free_cloned(dialog->pending_challenge);
  dialog->pending_challenge = NULL;
  (void)pjsip_tx_data_dec_ref(dialog->last_request);
  dialog->last_request = NULL;
  return pjsip_status(status);
}
static ls200_status pjsip_abort(void *c, void *i, void *d) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  (void)c;
  if (dialog == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  pjsip_remove_terminal_event(instance, dialog);
  pjsip_guard_untrack_dialog(instance, dialog);
  pjsip_terminate_invite_session(dialog, PJSIP_SC_REQUEST_TERMINATED);
  pjsip_remove_pending_fallback(instance, dialog);
  pjsip_remove_pending_response(instance, dialog);
  pjsip_clear_pending_response(dialog);
  dialog->pending_response_has_event = 0;
  (void)memset(&dialog->pending_response_event, 0,
               sizeof(dialog->pending_response_event));
  if (dialog->pending_challenge != NULL) {
    (void)pjsip_rx_data_free_cloned(dialog->pending_challenge);
    dialog->pending_challenge = NULL;
  }
  if (dialog->last_request != NULL) {
    (void)pjsip_tx_data_dec_ref(dialog->last_request);
    dialog->last_request = NULL;
  }
  return LS200_STATUS_OK;
}
static void pjsip_destroy_dialog(void *c, void *i, void *d) {
  ls200_pjsip_dialog *dialog = (ls200_pjsip_dialog *)d;
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)i;
  (void)c;
  if (dialog == NULL) return;
  (void)pjsip_abort(NULL, instance, dialog);
  pjsip_detach_dialog_binding(instance, dialog);
  if (dialog->dialog != NULL && dialog->session_retained != 0 &&
      instance != NULL) {
    (void)pjsip_dlg_dec_session(dialog->dialog, &instance->callback_module);
    dialog->session_retained = 0;
  } else if (dialog->dialog != NULL) {
    (void)pjsip_dlg_terminate(dialog->dialog);
  }
  if (dialog->transport != NULL) pjsip_transport_dec_ref(dialog->transport);
  pjsip_wipe(dialog, sizeof(*dialog));
  free(dialog);
}

static ls200_status pjsip_registration_status(
    void *context, void *opaque, ls200_sip_registration_status *out_status) {
  ls200_pjsip_instance *instance = (ls200_pjsip_instance *)opaque;
  (void)context;
  if (instance == NULL || out_status == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_status, 0, sizeof(*out_status));
  if (instance->registration_enabled == 0) return LS200_STATUS_OK;
  out_status->state = instance->registration_active != 0
      ? LS200_SIP_REGISTRATION_REGISTERED
      : instance->registration_status_code == 0
          ? LS200_SIP_REGISTRATION_REGISTERING
          : LS200_SIP_REGISTRATION_FAILED;
  out_status->status_code = instance->registration_status_code > 0 &&
      instance->registration_status_code <= 699
      ? (uint16_t)instance->registration_status_code : 0U;
  out_status->expires_seconds = instance->registration_expiration;
  return LS200_STATUS_OK;
}

static const ls200_sip_driver_vtable pjsip_vtable = {
  LS200_SIP_DRIVER_VTABLE_VERSION, sizeof(pjsip_vtable), pjsip_create,
  pjsip_destroy, pjsip_driver_resolve, pjsip_start_invite, pjsip_poll, pjsip_ack,
  pjsip_cancel, pjsip_bye, pjsip_reinvite, pjsip_answer, pjsip_reject,
  pjsip_digest, pjsip_abort, pjsip_destroy_dialog, pjsip_registration_status
};
