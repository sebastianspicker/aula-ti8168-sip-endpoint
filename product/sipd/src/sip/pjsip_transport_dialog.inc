static void pjsip_inv_send_ack(pjsip_inv_session *invite, pjsip_rx_data *rdata) {
  aula_pjsip_instance *instance = pjsip_callback_instance;
  aula_pjsip_dialog *dialog;
  aula_sip_transport_info next_hop;
  pjsip_tx_data *ack = NULL;
  aula_status policy_status;
  pj_status_t status;
  if (instance == NULL || invite == NULL || rdata == NULL || invite->dlg == NULL ||
      rdata->msg_info.cseq == NULL || instance->callback_module.id < 0)
    return;
  dialog = (aula_pjsip_dialog *)invite->dlg->mod_data[instance->callback_module.id];
  if (dialog == NULL) return;
  status = pjsip_inv_create_ack(invite, rdata->msg_info.cseq->cseq, &ack);
  if (status != PJ_SUCCESS) {
    pjsip_log_failure("ack-create", (int)status, 0);
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_GONE);
    pjsip_terminate_invite_session(dialog, PJSIP_SC_GONE);
    return;
  }
  policy_status = pjsip_authorize_dialog_destinations(instance, dialog,
                                                       &next_hop);
  if (policy_status != AULA_STATUS_OK) {
    pjsip_log_failure("ack-authorize", (int)policy_status, 0);
    (void)pjsip_tx_data_dec_ref(ack);
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_FORBIDDEN);
    pjsip_terminate_invite_session(dialog, PJSIP_SC_FORBIDDEN);
    return;
  }
  policy_status = pjsip_pin_dialog_request(ack, &next_hop);
  if (policy_status != AULA_STATUS_OK) {
    pjsip_log_failure("ack-pin", (int)policy_status, 0);
    (void)pjsip_tx_data_dec_ref(ack);
    /* A manual-ACK policy denial must also stop the invite usage. Otherwise
     * PJSIP may retain its timer/retransmission state after this callback. */
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_FORBIDDEN);
    pjsip_terminate_invite_session(dialog, PJSIP_SC_FORBIDDEN);
    return;
  }
  status = pjsip_inv_send_msg(invite, ack);
  if (status != PJ_SUCCESS) {
    pjsip_log_failure("ack-send", (int)status, 0);
    pjsip_queue_terminal_event(instance, dialog, PJSIP_SC_GONE);
  }
}

static aula_status pjsip_create_timer_invite(
    aula_pjsip_dialog *dialog, aula_bytes local_sdp) {
  char *sdp_text;
  pjmedia_sdp_session *sdp = NULL;
  pjsip_timer_setting timer;
  pj_status_t status;
  if (dialog == NULL || dialog->dialog == NULL || local_sdp.data == NULL ||
      local_sdp.length == 0U || local_sdp.length > AULA_SIPD_MAX_SDP_BYTES)
    return AULA_STATUS_INVALID_ARGUMENT;
  sdp_text = (char *)pj_pool_alloc(dialog->dialog->pool, local_sdp.length + 1U);
  if (sdp_text == NULL) return AULA_STATUS_INTERNAL_ERROR;
  (void)memcpy(sdp_text, local_sdp.data, local_sdp.length);
  sdp_text[local_sdp.length] = '\0';
  status = pjmedia_sdp_parse(dialog->dialog->pool, sdp_text,
                             (pj_size_t)local_sdp.length, &sdp);
  if (status != PJ_SUCCESS) return pjsip_operation_status("sdp-parse", status);
  status = pjsip_inv_create_uac(dialog->dialog, sdp, PJSIP_INV_SUPPORT_TIMER,
                                &dialog->invite);
  if (status != PJ_SUCCESS) return pjsip_operation_status("invite-create", status);
  status = pjsip_timer_setting_default(&timer);
  if (status != PJ_SUCCESS) return pjsip_operation_status("timer-default", status);
  timer.min_se = AULA_PJSIP_SESSION_TIMER_MIN_SECONDS;
  timer.sess_expires = AULA_PJSIP_SESSION_TIMER_EXPIRES_SECONDS;
  return pjsip_operation_status(
      "timer-init", pjsip_timer_init_session(dialog->invite, &timer));
}

#include "pjsip_fallback.inc"

/* Termination stops PJSIP's invite usage and its session timer. The function
 * can consume the final invite reference, so callers must not use the saved
 * pointer after it returns. */
static void pjsip_terminate_invite_session(aula_pjsip_dialog *dialog,
                                           int status_code) {
  pjsip_inv_session *invite;
  if (dialog == NULL || dialog->invite == NULL) return;
  dialog->closing = 1;
  invite = dialog->invite;
  dialog->invite = NULL;
  (void)pjsip_inv_terminate(invite, status_code, PJ_FALSE);
}

static void pjsip_free_new_dialog(aula_pjsip_instance *instance,
                                  aula_pjsip_dialog *dialog) {
  if (dialog == NULL) return;
  pjsip_remove_terminal_event(instance, dialog);
  pjsip_guard_untrack_dialog(instance, dialog);
  pjsip_terminate_invite_session(dialog, PJSIP_SC_REQUEST_TERMINATED);
  pjsip_detach_dialog_binding(instance, dialog);
  if (dialog->session_retained != 0) {
    (void)pjsip_dlg_dec_session(dialog->dialog, &instance->callback_module);
    dialog->session_retained = 0;
  }
  if (dialog->transport != NULL) pjsip_transport_dec_ref(dialog->transport);
  pjsip_wipe(dialog, sizeof(*dialog));
  free(dialog);
}

static aula_status pjsip_create_outbound_dialog(
    aula_pjsip_instance *instance, const aula_sip_invite_request *request,
    const pj_sockaddr *remote, int remote_length, aula_pjsip_dialog **out_dialog) {
  aula_pjsip_dialog *dialog = (aula_pjsip_dialog *)calloc(1U, sizeof(*dialog));
  pj_str_t target_text = pj_str((char *)request->target_uri);
  aula_status status;
  if (dialog == NULL) return AULA_STATUS_INTERNAL_ERROR;
  dialog->peer = request->peer;
  status = pjsip_operation_status(
      "transport-acquire", pjsip_acquire_authorized_transport(
          instance, request->target_uri, &request->peer, remote, remote_length,
          &dialog->transport));
  if (status == AULA_STATUS_OK)
    status = pjsip_operation_status(
        "dialog-create", pjsip_dlg_create_uac(
            pjsip_ua_instance(), &instance->local_uri, &instance->local_uri,
            &target_text, &target_text, &dialog->dialog));
  if (status != AULA_STATUS_OK) {
    if (dialog->transport != NULL) pjsip_transport_dec_ref(dialog->transport);
    free(dialog);
    return AULA_STATUS_IO_ERROR;
  }
  /* PJSIP's timer module creates refreshes internally. The currently pinned
   * source can retain a reliable selector's exact socket through those sends,
   * while UDP would resolve again before the final transmit callback. */
  if (request->peer.transport == AULA_TRANSPORT_TCP ||
      request->peer.transport == AULA_TRANSPORT_TLS) {
    aula_status timer_status = pjsip_create_timer_invite(dialog, request->local_sdp);
    if (timer_status != AULA_STATUS_OK) {
      /* INVITE creation retains a native dialog session. Terminating the
       * bare dialog would refuse while that session still exists. */
      if (dialog->invite != NULL)
        pjsip_terminate_invite_session(dialog, PJSIP_SC_REQUEST_TERMINATED);
      else
        (void)pjsip_dlg_terminate(dialog->dialog);
      pjsip_transport_dec_ref(dialog->transport);
      free(dialog);
      return timer_status;
    }
  }
  *out_dialog = dialog;
  return AULA_STATUS_OK;
}

static pj_status_t pjsip_attach_outbound_dialog(
    aula_pjsip_instance *instance, aula_pjsip_dialog *dialog) {
  pjsip_tpselector selector;
  pj_status_t status;
  (void)memset(&selector, 0, sizeof(selector));
  selector.type = PJSIP_TPSELECTOR_TRANSPORT;
  selector.u.transport = dialog->transport;
  /* Hold the outer lock while adding usage and retaining a session, matching
   * PJSIP invite-session ordering for a newly created low-level dialog. */
  pjsip_dlg_inc_lock(dialog->dialog);
  status = pjsip_dlg_add_usage(dialog->dialog, &instance->callback_module, dialog);
  if (status == PJ_SUCCESS) {
    status = pjsip_dlg_inc_session(dialog->dialog, &instance->callback_module);
    if (status == PJ_SUCCESS) dialog->session_retained = 1;
  }
  if (status == PJ_SUCCESS) status = pjsip_dlg_set_transport(dialog->dialog, &selector);
  if (status != PJ_SUCCESS && dialog->session_retained != 0) {
    (void)pjsip_dlg_dec_session(dialog->dialog, &instance->callback_module);
    dialog->session_retained = 0;
  }
  pjsip_dlg_dec_lock(dialog->dialog);
  return status;
}

static aula_status pjsip_create_tracked_outbound_dialog(
    aula_pjsip_instance *instance, const aula_sip_invite_request *request,
    aula_pjsip_dialog **out_dialog) {
  aula_pjsip_dialog *dialog = NULL;
  pj_sockaddr remote;
  int remote_length;
  aula_status status = pjsip_reauthorize_transport(instance, &request->peer);
  if (status != AULA_STATUS_OK) return status;
  status = pjsip_peer_to_sockaddr(&request->peer, &remote, &remote_length);
  if (status != AULA_STATUS_OK) return status;
  status = pjsip_create_outbound_dialog(instance, request, &remote,
                                        remote_length, &dialog);
  if (status != AULA_STATUS_OK) return status;
  if (pjsip_operation_status(
          "dialog-attach", pjsip_attach_outbound_dialog(instance, dialog)) !=
      AULA_STATUS_OK) {
    pjsip_free_new_dialog(instance, dialog);
    return AULA_STATUS_IO_ERROR;
  }
  status = pjsip_guard_track_dialog(instance, dialog);
  if (status != AULA_STATUS_OK) {
    pjsip_free_new_dialog(instance, dialog);
    return status;
  }
  *out_dialog = dialog;
  return AULA_STATUS_OK;
}

static aula_status pjsip_send_initial_invite(
    aula_pjsip_instance *instance, aula_pjsip_dialog *dialog) {
  pjsip_tx_data *invite = NULL;
  aula_sip_transport_info next_hop;
  aula_status status = pjsip_operation_status(
      "invite-build", pjsip_inv_invite(dialog->invite, &invite));
  if (status == AULA_STATUS_OK &&
      pjsip_authorize_initial_dialog_destinations(instance, dialog,
                                                   &next_hop) !=
          AULA_STATUS_OK) {
    (void)pjsip_tx_data_dec_ref(invite);
    status = AULA_STATUS_PERMISSION_DENIED;
  }
  if (status == AULA_STATUS_OK &&
      pjsip_pin_dialog_request(invite, &next_hop) != AULA_STATUS_OK) {
    (void)pjsip_tx_data_dec_ref(invite);
    status = AULA_STATUS_PERMISSION_DENIED;
  }
  if (status == AULA_STATUS_OK &&
      !pjsip_track_portable_request(dialog, invite, AULA_SIP_METHOD_INVITE)) {
    (void)pjsip_tx_data_dec_ref(invite);
    status = AULA_STATUS_STATE_ERROR;
  }
  if (status == AULA_STATUS_OK)
    status = pjsip_operation_status(
        "invite-send", pjsip_inv_send_msg(dialog->invite, invite));
  dialog->last_request_method = AULA_SIP_METHOD_INVITE;
  return status;
}

static aula_status pjsip_start_invite(void *context, void *opaque,
                                       const aula_sip_invite_request *request,
                                       void **out_dialog) {
  aula_pjsip_instance *instance = (aula_pjsip_instance *)opaque;
  aula_pjsip_dialog *dialog = NULL;
  aula_status status;
  (void)context;
  if (instance == NULL || request == NULL || out_dialog == NULL ||
      *out_dialog != NULL || request->cseq_number == 0U ||
      request->cseq_number > (uint32_t)INT32_MAX)
    return AULA_STATUS_INVALID_ARGUMENT;
  status = pjsip_create_tracked_outbound_dialog(instance, request, &dialog);
  if (status != AULA_STATUS_OK) return status;
  /* The portable adapter owns monotonically increasing CSeq identity. PJSIP
   * otherwise seeds a new dialog with a random CSeq and rewrites the request
   * while sending it, which would break transaction correlation at the
   * dependency boundary. */
  dialog->dialog->local.cseq = (pj_int32_t)request->cseq_number;
  (void)memcpy(dialog->initial_offer, request->local_sdp.data,
               request->local_sdp.length);
  dialog->initial_offer_length = request->local_sdp.length;
  dialog->initial_invite_cseq = request->cseq_number;
  if (dialog->invite != NULL) {
    status = pjsip_send_initial_invite(instance, dialog);
  } else {
    status = pjsip_send_dialog_request(instance, dialog, &pjsip_invite_method,
                                       AULA_SIP_METHOD_INVITE,
                                       (int)request->cseq_number,
                                       request->local_sdp);
  }
  if (status != AULA_STATUS_OK) {
    pjsip_free_new_dialog(instance, dialog);
    return status;
  }
  *out_dialog = dialog;
  return AULA_STATUS_OK;
}

static aula_status pjsip_poll(void *context, void *opaque, aula_deadline deadline) {
  aula_pjsip_instance *instance = (aula_pjsip_instance *)opaque;
  pj_time_val timeout;
  pj_status_t event_status;
  uint64_t now;
  uint64_t remaining;
  (void)context;
  if (instance == NULL || deadline.monotonic_ns == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_platform_monotonic_now(&now) != AULA_STATUS_OK) return AULA_STATUS_IO_ERROR;
  {
    aula_status readiness_status = pjsip_media_readiness_arm(instance);
    if (readiness_status != AULA_STATUS_OK) {
      /* Registration/arm failures retain the endpoint's bounded polling
       * fallback; media remains owned and consumed by the media session. */
      pjsip_media_readiness_cancel_instance(instance);
    }
  }
  remaining = now >= deadline.monotonic_ns ? 0U : deadline.monotonic_ns - now;
  if (remaining > UINT64_C(20000000)) remaining = UINT64_C(20000000);
  timeout.sec = (long)(remaining / UINT64_C(1000000000));
  timeout.msec = (long)((remaining % UINT64_C(1000000000)) / UINT64_C(1000000));
  event_status = pjsip_endpt_handle_events(instance->endpoint, &timeout);
  if (event_status == PJ_STATUS_FROM_OS(EINTR)) return AULA_STATUS_AGAIN;
  if (event_status != PJ_SUCCESS) return pjsip_status(event_status);
  {
    aula_status response_status = pjsip_drain_pending_responses(instance);
    if (response_status != AULA_STATUS_OK) {
      pjsip_log_failure("poll-responses", (int)response_status, 0);
      return response_status;
    }
  }
  {
    aula_status terminal_status = pjsip_drain_terminal_events(instance);
    if (terminal_status != AULA_STATUS_OK) {
      pjsip_log_failure("poll-terminal", (int)terminal_status, 0);
      return terminal_status;
    }
  }
  while (instance->fallback_head != NULL) {
    aula_pjsip_dialog *dialog = instance->fallback_head;
    aula_status status;
    instance->fallback_head = dialog->fallback_next;
    dialog->fallback_next = NULL;
    status = pjsip_retry_pending_fallback(instance, dialog);
    if (status != AULA_STATUS_OK) {
      pjsip_log_failure("poll-fallback", (int)status, 0);
      return status;
    }
  }
  return AULA_STATUS_OK;
}
