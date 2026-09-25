#include "media_control.h"

/* Only fixed operation labels and numeric codes cross this diagnostic path.
 * Never include SIP URIs, payloads, peer addresses, or credential material. */
static void pjsip_log_failure(const char *stage, int code, int os_error) {
  (void)fprintf(stderr,
                 "ls200-sipd: sip-stage=%s code=%d os_error=%d\n",
                 stage, code, os_error);
  (void)fflush(stderr);
}

static void pjsip_log_invite_disconnected(int state, int cause,
                                           int local_closing) {
  (void)fprintf(
      stderr,
      "ls200-sipd: sip-stage=invite-disconnected state=%d cause=%d local_closing=%d\n",
      state, cause, local_closing);
  (void)fflush(stderr);
}

static void pjsip_log_uri_metadata(int code, int port_present,
                                   int user_present, int lr_present,
                                   int maddr_present, int inherited_transport,
                                   int actual_transport) {
  (void)fprintf(
      stderr,
      "ls200-sipd: sip-stage=authorize-uri-metadata code=%d port_present=%d user_present=%d lr=%d maddr_present=%d inherited_transport=%d actual_transport=%d\n",
      code, port_present, user_present, lr_present, maddr_present,
      inherited_transport, actual_transport);
  (void)fflush(stderr);
}

static void pjsip_log_peer_mismatch(
    const ls200_sip_transport_info *resolved,
    const ls200_sip_transport_info *pinned) {
  int address_equal = resolved->remote_address_length ==
                          pinned->remote_address_length &&
      memcmp(resolved->remote_address, pinned->remote_address,
             resolved->remote_address_length) == 0;
  (void)fprintf(
      stderr,
      "ls200-sipd: sip-stage=authorize-dialog-peer-match code=%d transport_equal=%d address_equal=%d port_equal=%d resolved_port=%u pinned_port=%u\n",
      (int)LS200_STATUS_SECURITY_ERROR,
      resolved->transport == pinned->transport, address_equal,
      resolved->remote_port == pinned->remote_port,
      (unsigned int)resolved->remote_port, (unsigned int)pinned->remote_port);
  (void)fflush(stderr);
}

static ls200_status pjsip_operation_status(const char *stage, pj_status_t status) {
  if (status != PJ_SUCCESS) pjsip_log_failure(stage, (int)status, 0);
  return status == PJ_SUCCESS ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
}

/* Classify only a bounded, trusted in-dialog body. Never log body bytes. */
static int pjsip_body_has_picture_update(const pjsip_msg_body *body) {
  static const char token[] = "picture_fast_update";
  size_t offset;
  const uint8_t *data = (const uint8_t *)body->data;
  if (data == NULL || body->len > LS200_SIPD_MAX_SIP_MESSAGE_BYTES ||
      body->len < sizeof(token) - 1U) return 0;
  for (offset = 0U; offset <= body->len - (sizeof(token) - 1U); ++offset) {
    if (memcmp(data + offset, token, sizeof(token) - 1U) == 0) return 1;
  }
  return 0;
}

static void pjsip_observe_in_dialog_info(ls200_pjsip_dialog *dialog,
                                         const pjsip_rx_data *rdata) {
  const pjsip_msg *message = rdata->msg_info.msg;
  const pjsip_msg_body *body = message->body;
  int media_control;
  if (dialog->info_diagnostic_count >= 8U ||
      message->line.req.method.id != PJSIP_OTHER_METHOD ||
      pj_stricmp2(&message->line.req.method.name, "INFO") != 0) return;
  ++dialog->info_diagnostic_count;
  media_control = body != NULL &&
      pj_stricmp2(&body->content_type.type, "application") == 0 &&
      pj_stricmp2(&body->content_type.subtype, "media_control+xml") == 0;
  (void)fprintf(stderr,
      "ls200-sipd: sip-stage=info-metadata sample=%u body_present=%d "
      "media_control_xml=%d picture_fast_update=%d full_picture_supported=%d body_bytes=%u\n",
      dialog->info_diagnostic_count, body != NULL, media_control,
      media_control != 0 ? pjsip_body_has_picture_update(body) : 0,
      media_control != 0 ? ls200_sip_media_control_is_picture_update(
          (ls200_bytes){(const uint8_t *)body->data, (size_t)body->len}) : 0,
      body != NULL ? (unsigned int)body->len : 0U);
}
