/* Private native fallback queue. This is included only by pjsip.c after the
 * native sockaddr and transport-acquisition helpers are available. */
static ls200_status pjsip_authorize_fallback_transport(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    const ls200_sip_transport_info *peer) {
  ls200_sip_event event;
  ls200_sip_dialog_update update;
  if (instance == NULL || dialog == NULL || !ls200_sip_transport_is_valid(peer))
    return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&event, 0, sizeof(event));
  (void)memset(&update, 0, sizeof(update));
  event.kind = LS200_SIP_EVENT_TRANSPORT_FALLBACK;
  event.transport = *peer;
  /* Publish PJSIP's next CSeq before the replacement INVITE is built. */
  update.local_cseq = dialog->dialog == NULL || dialog->dialog->local.cseq <= 0
      ? 0U : (uint32_t)dialog->dialog->local.cseq;
  event.dialog_update = &update;
  return pjsip_emit_event(instance, dialog, &event);
}

static int pjsip_fallback_is_eligible(const ls200_pjsip_instance *instance,
                                      const ls200_pjsip_dialog *dialog,
                                      const pjsip_transaction *transaction) {
  return instance != NULL && dialog != NULL && transaction != NULL &&
      instance->profile == LS200_ZOOM_PROFILE_PRIVATE_LAB &&
      instance->allow_tcp_fallback != 0 &&
      dialog->peer.transport == LS200_TRANSPORT_UDP &&
      dialog->fallback_attempts == 0U && dialog->fallback_pending == 0 &&
      dialog->initial_invite_cseq != 0U &&
      transaction->method.id == PJSIP_INVITE_METHOD && transaction->cseq > 0 &&
      (uint32_t)transaction->cseq == dialog->initial_invite_cseq;
}

static int pjsip_schedule_tcp_fallback(ls200_pjsip_instance *instance,
                                       ls200_pjsip_dialog *dialog,
                                       pjsip_transaction *transaction) {
  ls200_sip_transport_info peer;
  if (!pjsip_fallback_is_eligible(instance, dialog, transaction)) return 0;
  peer = dialog->peer;
  peer.transport = LS200_TRANSPORT_TCP;
  /* Consume the sole retry before authorization so neither a denial nor a
   * TCP failure can cause another cleartext connection attempt. */
  ++dialog->fallback_attempts;
  if (pjsip_authorize_fallback_transport(instance, dialog, &peer) !=
      LS200_STATUS_OK)
    return 0;
  dialog->fallback_peer = peer;
  dialog->fallback_pending = 1;
  dialog->fallback_next = instance->fallback_head;
  instance->fallback_head = dialog;
  return 1;
}

static void pjsip_remove_pending_fallback(ls200_pjsip_instance *instance,
                                          ls200_pjsip_dialog *dialog) {
  ls200_pjsip_dialog **cursor;
  if (instance == NULL || dialog == NULL) return;
  for (cursor = &instance->fallback_head; *cursor != NULL;
       cursor = &(*cursor)->fallback_next) {
    if (*cursor == dialog) {
      *cursor = dialog->fallback_next;
      break;
    }
  }
  dialog->fallback_next = NULL;
  dialog->fallback_pending = 0;
}

static ls200_status pjsip_select_fallback_transport(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog) {
  pjsip_transport *replacement = NULL;
  pjsip_transport *previous;
  pjsip_tpselector selector;
  pj_sockaddr remote;
  int remote_length;
  ls200_status status;
  if (instance == NULL || dialog == NULL || dialog->fallback_pending == 0 ||
      dialog->fallback_peer.transport != LS200_TRANSPORT_TCP)
    return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_peer_to_sockaddr(&dialog->fallback_peer, &remote, &remote_length);
  if (status != LS200_STATUS_OK) return status;
  if (pjsip_acquire_authorized_transport(instance, NULL, &dialog->fallback_peer,
                                         &remote, remote_length,
                                         &replacement) != PJ_SUCCESS)
    return LS200_STATUS_IO_ERROR;
  (void)memset(&selector, 0, sizeof(selector));
  selector.type = PJSIP_TPSELECTOR_TRANSPORT;
  selector.u.transport = replacement;
  pjsip_dlg_inc_lock(dialog->dialog);
  status = pjsip_dlg_set_transport(dialog->dialog, &selector) == PJ_SUCCESS
      ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
  pjsip_dlg_dec_lock(dialog->dialog);
  if (status != LS200_STATUS_OK) {
    pjsip_transport_dec_ref(replacement);
    return status;
  }
  previous = dialog->transport;
  dialog->transport = replacement;
  dialog->peer = dialog->fallback_peer;
  dialog->fallback_pending = 0;
  if (previous != NULL) pjsip_transport_dec_ref(previous);
  return LS200_STATUS_OK;
}

static ls200_status pjsip_retry_pending_fallback(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog) {
  ls200_status status;
  if (dialog == NULL || dialog->fallback_pending == 0) return LS200_STATUS_OK;
  status = pjsip_select_fallback_transport(instance, dialog);
  if (status != LS200_STATUS_OK) return status;
  return pjsip_send_dialog_request(instance, dialog, &pjsip_invite_method,
                                   LS200_SIP_METHOD_INVITE,
                                   dialog->dialog == NULL ? 0 :
                                       (int)dialog->dialog->local.cseq,
                                   (ls200_bytes){dialog->initial_offer,
                                                 dialog->initial_offer_length});
}
