/* Low-level PJLIB/PJSIP only. All pj_* objects remain private behind the
 * ls200_sip_driver_vtable boundary. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pjlib-util.h>
#include <pjlib.h>
#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_timer.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjmedia/sdp.h>
#include <pjsip/sip_transport_tcp.h>
#if defined(PJ_HAS_SSL_SOCK) && PJ_HAS_SSL_SOCK
#include <pjsip/sip_transport_tls.h>
#endif
#include <pjsip/sip_transport_udp.h>
#include <stdio.h>
#include <stdlib.h>

#define LS200_PJSIP_KEEP_ALIVE_INTERVAL_SECONDS 90L
#define LS200_PJSIP_SESSION_TIMER_MIN_SECONDS 90U
#define LS200_PJSIP_SESSION_TIMER_EXPIRES_SECONDS 180U
#define LS200_PJSIP_DIALOG_KEY_BYTES 256U

typedef struct ls200_pjsip_dialog {
  pjsip_dialog *dialog;
  pjsip_inv_session *invite;
  pjsip_transport *transport;
  ls200_sip_transport_info peer;
  pjsip_tx_data *pending_response;
  pjsip_transaction *pending_request_transaction;
  pjsip_rx_data *pending_invite_request;
  int pending_response_is_invite;
  struct ls200_pjsip_dialog *pending_response_next;
  int pending_response_queued;
  int pending_response_has_event;
  ls200_sip_event pending_response_event;
  pjsip_rx_data *pending_challenge;
  pjsip_tx_data *last_request;
  ls200_sip_transport_info fallback_peer;
  int session_retained;
  int closing;
  int guard_tracked;
  unsigned int info_diagnostic_count;
  char guard_call_id[LS200_PJSIP_DIALOG_KEY_BYTES];
  char guard_local_tag[LS200_PJSIP_DIALOG_KEY_BYTES];
  struct ls200_pjsip_dialog *guard_next;
  struct ls200_pjsip_dialog *terminal_next;
  int terminal_queued;
  int native_status_code;
  unsigned fallback_attempts;
  int fallback_pending;
  struct ls200_pjsip_dialog *fallback_next;
  uint32_t initial_invite_cseq;
  uint32_t portable_request_cseq;
  ls200_sip_method portable_request_method;
  int portable_request_tracked;
  uint8_t initial_offer[LS200_SIPD_MAX_SDP_BYTES];
  size_t initial_offer_length;
  ls200_sip_method last_request_method;
  uint8_t event_sdp[LS200_SIPD_MAX_SDP_BYTES];
  size_t event_sdp_length;
  char realm[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char nonce[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char opaque[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char algorithm[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char qop[LS200_PJSIP_DIGEST_FIELD_BYTES];
  ls200_sip_digest_challenge challenge;
} ls200_pjsip_dialog;

typedef struct ls200_pjsip_media_watch ls200_pjsip_media_watch;

typedef struct ls200_pjsip_instance {
  ls200_sip_driver_event_sink sink;
  void *sink_context;
  pj_caching_pool caching_pool;
  pj_pool_t *pool;
  pjsip_endpoint *endpoint;
  pjsip_regc *registration;
  ls200_sip_credential_provider registration_credential_provider;
  void *registration_credential_context;
  uint32_t registration_digest_attempts;
  uint32_t registration_digest_limit;
  int registration_status_code;
  int registration_active;
  unsigned registration_expiration;
  int registration_enabled;
  pjsip_module receive_guard_module;
  pjsip_module callback_module;
  pjsip_inv_callback invite_callbacks;
  char local_uri_storage[LS200_PJSIP_REGISTRAR_BYTES];
  char auth_username[LS200_SIP_MAX_USERNAME_BYTES];
  pj_str_t local_uri;
  pjsip_transport *udp_transport;
  pjsip_tpfactory *tcp_factory;
#if defined(PJ_HAS_SSL_SOCK) && PJ_HAS_SSL_SOCK
  pjsip_tpfactory *tls_factory;
#endif
  int caching_pool_initialized;
  int pj_initialized;
  ls200_zoom_profile profile;
  ls200_sip_crc_route crc_route;
  int allow_tcp_fallback;
  ls200_pjsip_dialog *guard_head;
  ls200_pjsip_dialog *fallback_head;
  ls200_pjsip_dialog *pending_response_head;
  ls200_pjsip_dialog *pending_response_tail;
  ls200_pjsip_dialog *terminal_head;
  ls200_pjsip_dialog *terminal_tail;
  ls200_pjsip_media_watch *media_watches;
  size_t media_watch_count;
} ls200_pjsip_instance;

static ls200_pjsip_instance *pjsip_callback_instance;
static ls200_pjsip_instance *pjsip_active_instance;
static ls200_status pjsip_status(pj_status_t status);
static ls200_status pjsip_emit_event(ls200_pjsip_instance *instance,
                                     ls200_pjsip_dialog *dialog,
                                     ls200_sip_event *event);
static pj_bool_t pjsip_on_rx_guard(pjsip_rx_data *rdata);
static pj_bool_t pjsip_on_rx_response(pjsip_rx_data *rdata);
static pj_status_t pjsip_on_tx_request(pjsip_tx_data *request);
static void pjsip_on_tsx_state(pjsip_transaction *transaction,
                               pjsip_event *event);
static void pjsip_inv_state_changed(pjsip_inv_session *invite, pjsip_event *event);
static void pjsip_inv_send_ack(pjsip_inv_session *invite, pjsip_rx_data *rdata);
static pj_status_t pjsip_inv_reinvite_received(
    pjsip_inv_session *invite, const pjmedia_sdp_session *offer,
    pjsip_rx_data *rdata);
static pjsip_redirect_op pjsip_inv_stop_redirect(pjsip_inv_session *invite,
                                                 const pjsip_uri *target,
                                                 const pjsip_event *event);
static void pjsip_terminate_invite_session(ls200_pjsip_dialog *dialog,
                                           int status_code);

#include "pjsip_readiness.inc"

#include "pjsip_callbacks_queues.inc"

static pj_bool_t pjsip_on_rx_request(pjsip_rx_data *rdata);
static int pjsip_schedule_tcp_fallback(ls200_pjsip_instance *instance,
                                       ls200_pjsip_dialog *dialog,
                                       pjsip_transaction *transaction);

static ls200_sip_method pjsip_method_from_native(const pjsip_method *method) {
  if (method == NULL) return LS200_SIP_METHOD_INVITE;
  if (method->id == PJSIP_ACK_METHOD) return LS200_SIP_METHOD_ACK;
  if (method->id == PJSIP_CANCEL_METHOD) return LS200_SIP_METHOD_CANCEL;
  if (method->id == PJSIP_BYE_METHOD) return LS200_SIP_METHOD_BYE;
  return LS200_SIP_METHOD_INVITE;
}

static ls200_sip_method pjsip_response_method(
    const ls200_pjsip_dialog *dialog, const pjsip_cseq_hdr *cseq) {
  if (cseq != NULL && cseq->method.id == PJSIP_INVITE_METHOD && dialog != NULL)
    return dialog->last_request_method;
  return cseq == NULL ? LS200_SIP_METHOD_INVITE :
         pjsip_method_from_native(&cseq->method);
}

static int pjsip_native_method_matches_portable(
    const pjsip_method *native, ls200_sip_method portable) {
  if (native == NULL) return 0;
  if (portable == LS200_SIP_METHOD_REINVITE)
    return native->id == PJSIP_INVITE_METHOD;
  return pjsip_method_from_native(native) == portable;
}

static int pjsip_response_is_portable(
    const ls200_pjsip_dialog *dialog, const pjsip_cseq_hdr *cseq) {
  if (dialog == NULL || cseq == NULL || cseq->cseq <= 0) return 0;
  if (dialog->portable_request_tracked != 0 &&
      (uint32_t)cseq->cseq == dialog->portable_request_cseq &&
      pjsip_native_method_matches_portable(
          &cseq->method, dialog->portable_request_method))
    return 1;
  /* CANCEL also completes the original INVITE transaction with 487. */
  return dialog->portable_request_method == LS200_SIP_METHOD_CANCEL &&
      cseq->method.id == PJSIP_INVITE_METHOD &&
      (uint32_t)cseq->cseq == dialog->initial_invite_cseq;
}

static int pjsip_transaction_is_portable(
    const ls200_pjsip_dialog *dialog, const pjsip_transaction *transaction) {
  pjsip_cseq_hdr synthetic;
  if (transaction == NULL || transaction->cseq <= 0) return 0;
  (void)memset(&synthetic, 0, sizeof(synthetic));
  synthetic.cseq = transaction->cseq;
  synthetic.method = transaction->method;
  return pjsip_response_is_portable(dialog, &synthetic);
}

static int pjsip_track_portable_request(ls200_pjsip_dialog *dialog,
                                        const pjsip_tx_data *request,
                                        ls200_sip_method method) {
  pjsip_cseq_hdr *cseq;
  if (dialog == NULL || request == NULL || request->msg == NULL) return 0;
  cseq = (pjsip_cseq_hdr *)
      pjsip_msg_find_hdr(request->msg, PJSIP_H_CSEQ, NULL);
  if (cseq == NULL || cseq->cseq <= 0 ||
      !pjsip_native_method_matches_portable(&cseq->method, method))
    return 0;
  dialog->portable_request_cseq = (uint32_t)cseq->cseq;
  dialog->portable_request_method = method;
  dialog->portable_request_tracked = 1;
  return 1;
}

static int pjsip_copy_text(char *destination, size_t capacity, pj_str_t text) {
  if (destination == NULL || text.ptr == NULL || text.slen <= 0 ||
      (size_t)text.slen >= capacity) return 0;
  (void)memcpy(destination, text.ptr, (size_t)text.slen);
  destination[text.slen] = '\0';
  return 1;
}

static int pjsip_is_sdp_body(const pjsip_msg_body *body) {
  return body != NULL && body->data != NULL && body->len != 0U &&
      body->len <= LS200_SIPD_MAX_SDP_BYTES &&
      pj_stricmp2(&body->content_type.type, "application") == 0 &&
      pj_stricmp2(&body->content_type.subtype, "sdp") == 0;
}

static ls200_status pjsip_copy_sdp(ls200_pjsip_dialog *dialog,
                                   const pjsip_msg_body *body,
                                   ls200_bytes *out_sdp) {
  if (dialog == NULL || out_sdp == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  dialog->event_sdp_length = 0U;
  out_sdp->data = NULL;
  out_sdp->length = 0U;
  if (body == NULL) return LS200_STATUS_OK;
  if (!pjsip_is_sdp_body(body)) return LS200_STATUS_INVALID_DATA;
  (void)memcpy(dialog->event_sdp, body->data, body->len);
  dialog->event_sdp_length = body->len;
  out_sdp->data = dialog->event_sdp;
  out_sdp->length = dialog->event_sdp_length;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_transport_from_rdata(
    const pjsip_rx_data *rdata, ls200_sip_transport_info *transport) {
  unsigned address_length;
  if (rdata == NULL || transport == NULL || rdata->tp_info.transport == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(transport, 0, sizeof(*transport));
  switch (rdata->tp_info.transport->key.type) {
    case PJSIP_TRANSPORT_UDP: transport->transport = LS200_TRANSPORT_UDP; break;
    case PJSIP_TRANSPORT_TCP: transport->transport = LS200_TRANSPORT_TCP; break;
    case PJSIP_TRANSPORT_TLS: transport->transport = LS200_TRANSPORT_TLS; break;
    default: return LS200_STATUS_UNSUPPORTED;
  }
  address_length = pj_sockaddr_get_addr_len(&rdata->pkt_info.src_addr);
  if (address_length != 4U && address_length != 16U) return LS200_STATUS_INVALID_DATA;
  transport->remote_address_length = (uint8_t)address_length;
  (void)memcpy(transport->remote_address,
               pj_sockaddr_get_addr(&rdata->pkt_info.src_addr), address_length);
  transport->remote_port = pj_sockaddr_get_port(&rdata->pkt_info.src_addr);
  transport->local_port = pj_sockaddr_get_port(&rdata->tp_info.transport->local_addr);
  transport->rport = transport->remote_port;
  return ls200_sip_transport_is_valid(transport) ? LS200_STATUS_OK :
                                                  LS200_STATUS_INVALID_DATA;
}

static void pjsip_fill_transaction(ls200_sip_transaction_identity *transaction,
                                   void *handle, ls200_sip_transaction_role role,
                                   ls200_sip_method method,
                                   const pjsip_cseq_hdr *cseq) {
  (void)memset(transaction, 0, sizeof(*transaction));
  transaction->handle = handle;
  transaction->role = role;
  transaction->method = method;
  transaction->cseq_number = cseq == NULL || cseq->cseq <= 0 ? 0U :
      (uint32_t)cseq->cseq;
}

static ls200_status pjsip_emit_event(ls200_pjsip_instance *instance,
                                     ls200_pjsip_dialog *dialog,
                                     ls200_sip_event *event) {
  ls200_sip_driver_event driver_event;
  if (instance == NULL || dialog == NULL || event == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&driver_event, 0, sizeof(driver_event));
  driver_event.driver_dialog = dialog;
  driver_event.event = *event;
  return instance->sink(instance->sink_context, &driver_event);
}


static ls200_status pjsip_status(pj_status_t status) {
  return status == PJ_SUCCESS ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
}

static void pjsip_wipe(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length-- > 0U) *cursor++ = 0U;
}

static void pjsip_unregister_instance(ls200_pjsip_instance *instance) {
  pjsip_tx_data *request = NULL;
  pj_time_val timeout = {0, 25};
  unsigned attempt;
  if (instance == NULL || instance->endpoint == NULL ||
      instance->registration == NULL || instance->registration_active == 0)
    return;
  if (pjsip_regc_unregister(instance->registration, &request) != PJ_SUCCESS ||
      request == NULL ||
      pjsip_regc_send(instance->registration, request) != PJ_SUCCESS)
    return;
  /* Orderly shutdown gives only this contact a bounded opportunity to expire.
   * Failure never prevents local teardown and unregister_all is never used. */
  for (attempt = 0U; attempt < 12U && instance->registration_active != 0;
       ++attempt)
    (void)pjsip_endpt_handle_events(instance->endpoint, &timeout);
}

static void pjsip_release_instance(ls200_pjsip_instance *instance) {
  if (instance == NULL) return;
  pjsip_media_readiness_cancel_instance(instance);
  if (pjsip_callback_instance == instance) pjsip_callback_instance = NULL;
  if (pjsip_active_instance == instance) pjsip_active_instance = NULL;
  pjsip_unregister_instance(instance);
  if (instance->endpoint != NULL && instance->receive_guard_module.id >= 0)
    (void)pjsip_endpt_unregister_module(instance->endpoint,
                                        &instance->receive_guard_module);
  if (instance->endpoint != NULL && instance->callback_module.id >= 0)
    (void)pjsip_endpt_unregister_module(instance->endpoint, &instance->callback_module);
  if (instance->registration != NULL) pjsip_regc_destroy(instance->registration);
  if (instance->endpoint != NULL) pjsip_endpt_destroy(instance->endpoint);
  if (instance->pool != NULL) pj_pool_release(instance->pool);
  if (instance->caching_pool_initialized != 0)
    pj_caching_pool_destroy(&instance->caching_pool);
  if (instance->pj_initialized != 0) pj_shutdown();
  pjsip_wipe(instance, sizeof(*instance));
  free(instance);
}

static ls200_status pjsip_initialize_stack(ls200_pjsip_instance *instance) {
  pj_status_t status;
  status = pj_init();
  if (status != PJ_SUCCESS) return pjsip_status(status);
  instance->pj_initialized = 1;
  status = pjlib_util_init();
  if (status != PJ_SUCCESS) return pjsip_status(status);
  pj_caching_pool_init(&instance->caching_pool, NULL, 0U);
  instance->caching_pool_initialized = 1;
  instance->pool = pj_pool_create(&instance->caching_pool.factory, "ls200-sip", 8192U,
                                  8192U, NULL);
  if (instance->pool == NULL) return LS200_STATUS_INTERNAL_ERROR;
  instance->media_watches = (ls200_pjsip_media_watch *)pj_pool_calloc(
      instance->pool, LS200_PJSIP_MEDIA_WATCH_LIMIT,
      sizeof(*instance->media_watches));
  if (instance->media_watches == NULL) return LS200_STATUS_INTERNAL_ERROR;
  {
    size_t index;
    for (index = 0U; index < LS200_PJSIP_MEDIA_WATCH_LIMIT; ++index) {
      instance->media_watches[index].source_descriptor = -1;
      instance->media_watches[index].owned_descriptor = -1;
    }
  }
  status = pjsip_endpt_create(&instance->caching_pool.factory, "ls200-sip",
                              &instance->endpoint);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_tsx_layer_init_module(instance->endpoint);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_ua_init_module(instance->endpoint, NULL);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  (void)memset(&instance->invite_callbacks, 0, sizeof(instance->invite_callbacks));
  instance->invite_callbacks.on_state_changed = &pjsip_inv_state_changed;
  instance->invite_callbacks.on_send_ack = &pjsip_inv_send_ack;
  instance->invite_callbacks.on_rx_reinvite = &pjsip_inv_reinvite_received;
  /* The invite usage never receives authorization to follow a redirect. */
  instance->invite_callbacks.on_redirected = &pjsip_inv_stop_redirect;
  status = pjsip_inv_usage_init(instance->endpoint, &instance->invite_callbacks);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_100rel_init_module(instance->endpoint);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_timer_init_module(instance->endpoint);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  (void)memset(&instance->callback_module, 0, sizeof(instance->callback_module));
  instance->callback_module.id = -1;
  instance->callback_module.name = pj_str("ls200-pjsip");
  instance->callback_module.priority = PJSIP_MOD_PRIORITY_APPLICATION;
  instance->callback_module.on_rx_request = &pjsip_on_rx_request;
  instance->callback_module.on_rx_response = &pjsip_on_rx_response;
  instance->callback_module.on_tx_request = &pjsip_on_tx_request;
  instance->callback_module.on_tsx_state = &pjsip_on_tsx_state;
  status = pjsip_endpt_register_module(instance->endpoint, &instance->callback_module);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  (void)memset(&instance->receive_guard_module, 0,
               sizeof(instance->receive_guard_module));
  instance->receive_guard_module.id = -1;
  instance->receive_guard_module.name = pj_str("ls200-pjsip-rx-guard");
  /* The guard must run before the transaction layer (16). It uses the
   * adapter-owned Call-ID/local-tag registry because UA association has not
   * happened yet, covering initial responses as well as established dialogs. */
  instance->receive_guard_module.priority = PJSIP_MOD_PRIORITY_TSX_LAYER - 1;
  instance->receive_guard_module.on_rx_request = &pjsip_on_rx_guard;
  instance->receive_guard_module.on_rx_response = &pjsip_on_rx_guard;
  status = pjsip_endpt_register_module(instance->endpoint,
                                       &instance->receive_guard_module);
  if (status != PJ_SUCCESS) {
    (void)pjsip_endpt_unregister_module(instance->endpoint, &instance->callback_module);
    return pjsip_status(status);
  }
  pjsip_callback_instance = instance;
  return pjsip_status(status);
}

static ls200_status pjsip_start_transports(
    ls200_pjsip_instance *instance, const ls200_pjsip_context *settings,
    const ls200_sip_endpoint_config *endpoint) {
  pj_status_t status;
  if (instance == NULL || settings == NULL || endpoint == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (settings->profile == LS200_ZOOM_PROFILE_PROXY_REGISTRATION &&
      (endpoint->enable_tls == 0 ||
       endpoint->preferred_transport != LS200_TRANSPORT_TLS))
    return LS200_STATUS_CONFIGURATION_ERROR;
  if ((endpoint->enable_tls != 0) !=
      (endpoint->preferred_transport == LS200_TRANSPORT_TLS))
    return LS200_STATUS_CONFIGURATION_ERROR;
  /* Pin the documented PJSIP 90-second default as an explicit production
   * policy before either connection-oriented transport factory is created.
   * UDP has no equivalent policy in this adapter. */
  pjsip_cfg()->tcp.keep_alive_interval = LS200_PJSIP_KEEP_ALIVE_INTERVAL_SECONDS;
  pjsip_cfg()->tls.keep_alive_interval = LS200_PJSIP_KEEP_ALIVE_INTERVAL_SECONDS;
  if (endpoint->enable_tls != 0) {
#if defined(PJ_HAS_SSL_SOCK) && PJ_HAS_SSL_SOCK
    pjsip_tls_setting tls;
    if (settings->tls_ca_file[0] == '\0') return LS200_STATUS_CONFIGURATION_ERROR;
    pjsip_tls_setting_default(&tls);
    tls.ca_list_file = pj_str((char *)settings->tls_ca_file);
    tls.verify_server = PJ_TRUE;
    tls.verify_client = PJ_FALSE;
    tls.require_client_cert = PJ_FALSE;
    tls.enable_renegotiation = PJ_FALSE;
    tls.proto = PJ_SSL_SOCK_PROTO_TLS1_2 | PJ_SSL_SOCK_PROTO_TLS1_3;
    tls.timeout.sec = (long)(endpoint->transaction_timeout_ms / 1000U);
    tls.timeout.msec = (long)(endpoint->transaction_timeout_ms % 1000U);
    status = pjsip_tls_transport_start(instance->endpoint, &tls, NULL, NULL, 1U,
                                       &instance->tls_factory);
    return pjsip_status(status);
#else
    (void)settings;
    return LS200_STATUS_UNSUPPORTED;
#endif
  }
  /* Bind one ephemeral local listener for each cleartext transport. The
   * selected peer transport still controls every outbound acquire; both
   * factories are needed to receive an in-dialog request on either lab path. */
  status = pjsip_udp_transport_start(instance->endpoint, NULL, NULL, 1U,
                                     &instance->udp_transport);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_tcp_transport_start(instance->endpoint, NULL, 1U,
                                     &instance->tcp_factory);
  return pjsip_status(status);
}

static ls200_status pjsip_register_proxy(ls200_pjsip_instance *instance,
                                         const ls200_pjsip_context *settings,
                                         const ls200_sip_endpoint_config *endpoint);

static ls200_status pjsip_attach_sdp(pjsip_tx_data *request, ls200_bytes sdp) {
  pj_str_t type = pj_str("application");
  pj_str_t subtype = pj_str("sdp");
  pj_str_t text;
  if (request == NULL || sdp.data == NULL || sdp.length == 0U ||
      sdp.length > LS200_SIPD_MAX_SDP_BYTES) return LS200_STATUS_INVALID_ARGUMENT;
  text.ptr = (char *)sdp.data;
  text.slen = (pj_ssize_t)sdp.length;
  request->msg->body = pjsip_msg_body_create(request->pool, &type, &subtype, &text);
  return request->msg->body == NULL ? LS200_STATUS_INTERNAL_ERROR : LS200_STATUS_OK;
}

static void pjsip_clear_pending_response(ls200_pjsip_dialog *dialog) {
  if (dialog == NULL) return;
  if (dialog->pending_response != NULL)
    (void)pjsip_tx_data_dec_ref(dialog->pending_response);
  if (dialog->pending_invite_request != NULL)
    (void)pjsip_rx_data_free_cloned(dialog->pending_invite_request);
  dialog->pending_response = NULL;
  dialog->pending_request_transaction = NULL;
  dialog->pending_invite_request = NULL;
  dialog->pending_response_is_invite = 0;
}
