/* Deferred response and native-terminal queues share pjsip.c's private
 * types and static helpers; keep this fragment included before callbacks. */
static void pjsip_remove_terminal_event(ls200_pjsip_instance *instance,
                                        ls200_pjsip_dialog *dialog) {
  ls200_pjsip_dialog *previous = NULL;
  ls200_pjsip_dialog *cursor;
  if (instance == NULL || dialog == NULL || dialog->terminal_queued == 0)
    return;
  cursor = instance->terminal_head;
  while (cursor != NULL && cursor != dialog) {
    previous = cursor;
    cursor = cursor->terminal_next;
  }
  if (cursor == NULL) {
    dialog->terminal_queued = 0;
    dialog->terminal_next = NULL;
    return;
  }
  if (previous == NULL)
    instance->terminal_head = cursor->terminal_next;
  else
    previous->terminal_next = cursor->terminal_next;
  if (instance->terminal_tail == cursor) instance->terminal_tail = previous;
  cursor->terminal_queued = 0;
  cursor->terminal_next = NULL;
}

static void pjsip_queue_terminal_event(ls200_pjsip_instance *instance,
                                       ls200_pjsip_dialog *dialog,
                                       int status_code) {
  if (instance == NULL || dialog == NULL || dialog->terminal_queued != 0)
    return;
  dialog->native_status_code = status_code;
  dialog->terminal_queued = 1;
  dialog->terminal_next = NULL;
  if (instance->terminal_tail == NULL)
    instance->terminal_head = dialog;
  else
    instance->terminal_tail->terminal_next = dialog;
  instance->terminal_tail = dialog;
}

static ls200_status pjsip_drain_terminal_events(
    ls200_pjsip_instance *instance) {
  while (instance != NULL && instance->terminal_head != NULL) {
    ls200_pjsip_dialog *dialog = instance->terminal_head;
    ls200_sip_event event;
    int status_code = dialog->native_status_code;
    pjsip_remove_terminal_event(instance, dialog);
    (void)memset(&event, 0, sizeof(event));
    event.kind = LS200_SIP_EVENT_SESSION_TERMINATED;
    event.transport = dialog->peer;
    event.reason_code = status_code == PJSIP_SC_REQUEST_TIMEOUT ?
        "session_timer_expired" : "native_session_terminated";
    /* The sink retires and frees dialog synchronously. It must be the last
     * operation involving this queue entry. */
    return pjsip_emit_event(instance, dialog, &event);
  }
  return LS200_STATUS_OK;
}

static void pjsip_detach_dialog_binding(ls200_pjsip_instance *instance,
                                         ls200_pjsip_dialog *dialog) {
  if (instance == NULL || dialog == NULL || dialog->dialog == NULL ||
      instance->callback_module.id < 0)
    return;
  /* A final response may synchronously retire the portable dialog while
   * PJSIP still owns the transaction that delivered it. Clear the native
   * callback binding before releasing the last session: later terminal
   * callbacks then ignore the retired dialog instead of dereferencing it. */
  (void)pjsip_dlg_set_mod_data(dialog->dialog, instance->callback_module.id, NULL);
}

/* Dialog usages run before PJSIP feeds a newly created UAS transaction its
 * first RX event. Sending a response synchronously from the usage callback
 * therefore re-enters a transaction that is still in NULL state. Queue the
 * already validated response and drain it after handle_events() returns, when
 * PJSIP has advanced the transaction to TRYING. */
static ls200_status pjsip_queue_pending_response(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog) {
  if (instance == NULL || dialog == NULL || dialog->pending_response == NULL ||
      dialog->pending_request_transaction == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (dialog->pending_response_queued != 0)
    return LS200_STATUS_STATE_ERROR;
  dialog->pending_response_queued = 1;
  dialog->pending_response_next = NULL;
  if (instance->pending_response_tail == NULL)
    instance->pending_response_head = dialog;
  else
    instance->pending_response_tail->pending_response_next = dialog;
  instance->pending_response_tail = dialog;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_create_queued_response(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    pjsip_transaction *transaction, pjsip_rx_data *rdata, int status_code,
    const ls200_sip_event *event_after_send) {
  pj_status_t status;
  if (instance == NULL || dialog == NULL || transaction == NULL || rdata == NULL ||
      status_code < 200 || status_code > 699 || dialog->pending_response != NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_dlg_create_response(dialog->dialog, rdata, status_code, NULL,
                                     &dialog->pending_response);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  dialog->pending_request_transaction = transaction;
  if (event_after_send != NULL) {
    dialog->pending_response_event = *event_after_send;
    dialog->pending_response_has_event = 1;
  }
  return pjsip_queue_pending_response(instance, dialog);
}

static ls200_status pjsip_replace_and_queue_pending_response(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    int status_code) {
  pj_status_t status;
  if (instance == NULL || dialog == NULL || dialog->pending_response == NULL ||
      dialog->pending_request_transaction == NULL || status_code < 300 ||
      status_code > 699)
    return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_dlg_modify_response(dialog->dialog, dialog->pending_response,
                                     status_code, NULL);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  /* modify_response() adds a reference; the queue owns only the original. */
  (void)pjsip_tx_data_dec_ref(dialog->pending_response);
  return pjsip_queue_pending_response(instance, dialog);
}

static void pjsip_remove_pending_response(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog) {
  ls200_pjsip_dialog *previous = NULL;
  ls200_pjsip_dialog *cursor;
  if (instance == NULL || dialog == NULL || dialog->pending_response_queued == 0)
    return;
  cursor = instance->pending_response_head;
  while (cursor != NULL && cursor != dialog) {
    previous = cursor;
    cursor = cursor->pending_response_next;
  }
  if (cursor == NULL) {
    dialog->pending_response_queued = 0;
    dialog->pending_response_next = NULL;
    return;
  }
  if (previous == NULL)
    instance->pending_response_head = cursor->pending_response_next;
  else
    previous->pending_response_next = cursor->pending_response_next;
  if (instance->pending_response_tail == cursor)
    instance->pending_response_tail = previous;
  cursor->pending_response_queued = 0;
  cursor->pending_response_next = NULL;
}

static ls200_status pjsip_drain_pending_responses(
    ls200_pjsip_instance *instance) {
  while (instance != NULL && instance->pending_response_head != NULL) {
    ls200_pjsip_dialog *dialog = instance->pending_response_head;
    pjsip_tx_data *response = dialog->pending_response;
    pjsip_transaction *transaction = dialog->pending_request_transaction;
    ls200_sip_event event_after_send = dialog->pending_response_event;
    int emit_event = dialog->pending_response_has_event;
    int invite_response = dialog->pending_response_is_invite;
    pj_status_t status;
    pjsip_remove_pending_response(instance, dialog);
    /* Detach every dialog-owned reference before a send, callback, or early
     * return. Native disconnect may have queued terminal retirement and
     * cleared invite while this response was waiting. */
    dialog->pending_response = NULL;
    dialog->pending_request_transaction = NULL;
    dialog->pending_response_is_invite = 0;
    dialog->pending_response_has_event = 0;
    (void)memset(&dialog->pending_response_event, 0,
                 sizeof(dialog->pending_response_event));
    if (response == NULL || transaction == NULL) {
      if (response != NULL) (void)pjsip_tx_data_dec_ref(response);
      return LS200_STATUS_STATE_ERROR;
    }
    /* Both native response send paths consume the response reference. */
    if (invite_response != 0) {
      if (dialog->invite == NULL) {
        (void)pjsip_tx_data_dec_ref(response);
        continue;
      }
      status = pjsip_inv_send_msg(dialog->invite, response);
    } else {
      status = pjsip_dlg_send_response(dialog->dialog, transaction, response);
    }
    if (status != PJ_SUCCESS) return pjsip_status(status);
    /* The sink may synchronously retire and free the native dialog. Keep this
     * as the last operation involving it. */
    if (emit_event != 0)
      return pjsip_emit_event(instance, dialog, &event_after_send);
  }
  return LS200_STATUS_OK;
}
