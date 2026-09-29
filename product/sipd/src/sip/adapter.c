#include "aula_sipd/sip.h"

#include "adapter_internal.h"
#include "driver_internal.h"
#include "aula_sipd/platform.h"
#include "aula_sipd/sdp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void aula_sip_wipe(void *memory, size_t length) {
  volatile uint8_t *bytes = (volatile uint8_t *)memory;
  while (length-- > 0U) *bytes++ = 0U;
}

int aula_sip_copy(char *destination, size_t capacity, const char *source) {
  size_t length;
  if (destination == NULL || source == NULL || capacity == 0U) return 0;
  length = strlen(source);
  if (length == 0U || length >= capacity) return 0;
  (void)memcpy(destination, source, length + 1U);
  return 1;
}

int aula_sip_text_is_safe(const char *text, size_t maximum) {
  size_t index;
  if (text == NULL || text[0] == '\0' || strlen(text) >= maximum) return 0;
  for (index = 0U; text[index] != '\0'; ++index) {
    unsigned char c = (unsigned char)text[index];
    if (c <= 0x20U || c >= 0x7fU) return 0;
  }
  return 1;
}

int aula_sip_transport_is_valid(const aula_sip_transport_info *transport) {
  if (transport == NULL || transport->transport < AULA_TRANSPORT_UDP ||
      transport->transport > AULA_TRANSPORT_TLS || transport->local_port == 0U ||
      transport->remote_port == 0U ||
      (transport->remote_address_length != 4U && transport->remote_address_length != 16U)) return 0;
  return transport->received_address_length == 0U || transport->received_address_length == 4U ||
         transport->received_address_length == 16U;
}

static int sip_ipv4_is_restricted(const uint8_t address[4]) {
  return address[0] == 0U || address[0] == 10U || address[0] == 127U ||
      (address[0] == 100U && (address[1] & 0xc0U) == 0x40U) ||
      (address[0] == 169U && address[1] == 254U) ||
      (address[0] == 172U && (address[1] & 0xf0U) == 16U) ||
      (address[0] == 192U && address[1] == 168U) || address[0] >= 224U;
}

int aula_sip_transport_is_restricted_network(
    const aula_sip_transport_info *transport) {
  static const uint8_t zero[16] = {0U};
  static const uint8_t loopback[16] = {
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U};
  static const uint8_t mapped_prefix[12] = {
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xffU, 0xffU};
  const uint8_t *address;
  if (!aula_sip_transport_is_valid(transport)) return 1;
  address = transport->remote_address;
  if (transport->remote_address_length == 4U) {
    return sip_ipv4_is_restricted(address);
  }
  if (memcmp(address, mapped_prefix, sizeof(mapped_prefix)) == 0) {
    return sip_ipv4_is_restricted(address + sizeof(mapped_prefix));
  }
  if (memcmp(address, zero, sizeof(zero)) == 0 ||
      memcmp(address, loopback, sizeof(loopback)) == 0) return 1;
  return (address[0] & 0xfeU) == 0xfcU ||
      (address[0] == 0xfeU && (address[1] & 0xc0U) == 0x80U) ||
      address[0] == 0xffU;
}

int aula_sip_transport_matches(const aula_sip_transport_info *left,
                                const aula_sip_transport_info *right) {
  if (!aula_sip_transport_is_valid(left) || !aula_sip_transport_is_valid(right) ||
      left->transport != right->transport || left->remote_port != right->remote_port ||
      left->remote_address_length != right->remote_address_length)
    return 0;
  return memcmp(left->remote_address, right->remote_address,
                left->remote_address_length) == 0;
}

aula_status aula_sip_authorize_peer(aula_sip_adapter *adapter,
                                      const aula_sip_transport_info *peer,
                                      int is_fallback) {
  if (adapter == NULL || !aula_sip_transport_is_valid(peer)) return AULA_STATUS_INVALID_ARGUMENT;
  if (is_fallback != 0 && adapter->allow_tcp_fallback == 0) return AULA_STATUS_PERMISSION_DENIED;
  if (adapter->network_scope_authorizer == NULL) return AULA_STATUS_PERMISSION_DENIED;
  return adapter->network_scope_authorizer(adapter->network_scope_context, peer, is_fallback != 0);
}

static void aula_sip_emit_unavailable(aula_sip_adapter *adapter) {
  aula_sip_event event;
  if (adapter == NULL || adapter->event_callback == NULL || adapter->unavailable_reported) return;
  (void)memset(&event, 0, sizeof(event));
  event.kind = AULA_SIP_EVENT_TRANSPORT_ERROR;
  event.reason_code = "sip_driver_unsupported";
  adapter->unavailable_reported = 1;
  adapter->event_callback(adapter->event_context, &event);
}

static aula_status aula_sip_deadline_after(uint32_t milliseconds, aula_deadline *out_deadline) {
  uint64_t now;
  if (out_deadline == NULL || aula_platform_monotonic_now(&now) != AULA_STATUS_OK) return AULA_STATUS_IO_ERROR;
  out_deadline->monotonic_ns = now + ((uint64_t)milliseconds * UINT64_C(1000000));
  return AULA_STATUS_OK;
}

static aula_status aula_sip_validate_sdp(aula_bytes sdp) {
  aula_sdp_session *session = NULL;
  aula_status status = aula_sdp_parse_offer(sdp, &session);
  if (status == AULA_STATUS_OK) aula_sdp_session_destroy(session);
  return status;
}

static aula_status aula_sip_validate_answer_sdp(aula_bytes sdp) {
  aula_sdp_session *session = NULL;
  aula_status status = aula_sdp_parse_answer(sdp, &session);
  if (status == AULA_STATUS_OK) aula_sdp_session_destroy(session);
  return status;
}

aula_sip_dialog *aula_sip_find_dialog(aula_sip_adapter *adapter,
                                        void *driver_dialog) {
  aula_sip_dialog *dialog;
  for (dialog = adapter == NULL ? NULL : adapter->dialogs; dialog != NULL; dialog = dialog->next) {
    if (dialog->driver_dialog == driver_dialog) return dialog;
  }
  return NULL;
}

static size_t aula_sip_dialog_count(const aula_sip_adapter *adapter) {
  const aula_sip_dialog *dialog;
  size_t count = 0U;
  for (dialog = adapter == NULL ? NULL : adapter->dialogs; dialog != NULL;
       dialog = dialog->next)
    ++count;
  return count;
}

static void aula_sip_unlink_dialog(aula_sip_dialog *dialog) {
  aula_sip_dialog **cursor;
  if (dialog == NULL || dialog->adapter == NULL) return;
  for (cursor = &dialog->adapter->dialogs; *cursor != NULL; cursor = &(*cursor)->next) {
    if (*cursor == dialog) { *cursor = dialog->next; break; }
  }
  dialog->adapter = NULL;
  dialog->next = NULL;
}

int aula_sip_transaction_is_valid(
    const aula_sip_transaction_identity *transaction) {
  return transaction != NULL && transaction->handle != NULL &&
         (transaction->role == AULA_SIP_TRANSACTION_ROLE_CLIENT ||
          transaction->role == AULA_SIP_TRANSACTION_ROLE_SERVER) &&
         transaction->method >= AULA_SIP_METHOD_INVITE &&
         transaction->method <= AULA_SIP_METHOD_REINVITE && transaction->cseq_number != 0U;
}

#include "adapter_configuration.inc"

aula_status aula_sip_adapter_create(const aula_sip_endpoint_config *config,
                                      aula_sip_adapter **out_adapter) {
  aula_sip_adapter *adapter;
  if (config == NULL || out_adapter == NULL || *out_adapter != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (!adapter_config_is_valid(config)) return AULA_STATUS_CONFIGURATION_ERROR;
  adapter = (aula_sip_adapter *)calloc(1U, sizeof(*adapter));
  if (adapter == NULL) return AULA_STATUS_INTERNAL_ERROR;
  (void)aula_sip_copy(adapter->outbound_uri, sizeof(adapter->outbound_uri), config->outbound_uri);
  if (config->auth_username != NULL) (void)aula_sip_copy(adapter->username, sizeof(adapter->username), config->auth_username);
  if (config->auth_secret_file != NULL && !aula_sip_copy(adapter->secret_path, sizeof(adapter->secret_path), config->auth_secret_file)) { free(adapter); return AULA_STATUS_CONFIGURATION_ERROR; }
  adapter->transaction_timeout_ms = config->transaction_timeout_ms == 0U ? 32000U : config->transaction_timeout_ms;
  adapter->max_retransmissions = config->max_retransmissions == 0U ? 7U : config->max_retransmissions;
  adapter->max_digest_retries = config->max_digest_retries == 0U ? 2U : config->max_digest_retries;
  adapter->allow_tcp_fallback = config->allow_tcp_fallback != 0;
  adapter->event_callback = config->event_callback;
  adapter->event_context = config->event_context;
  adapter->credential_provider = config->credential_provider;
  adapter->credential_context = config->credential_context;
  adapter->network_scope_authorizer = config->network_scope_authorizer;
  adapter->network_scope_context = config->network_scope_context;
  adapter->endpoint = *config;
  adapter_set_limit_defaults(adapter);
  adapter->endpoint.outbound_uri = adapter->outbound_uri;
  adapter->endpoint.auth_username = adapter->username[0] == '\0' ? NULL : adapter->username;
  adapter->endpoint.auth_secret_file = adapter->secret_path[0] == '\0' ? NULL : adapter->secret_path;
  adapter->state = AULA_SIP_ADAPTER_UNAVAILABLE;
  *out_adapter = adapter;
  return AULA_STATUS_OK;
}

aula_status aula_sip_adapter_attach_driver(aula_sip_adapter *adapter, aula_sip_driver *driver) {
  aula_status status;
  if (adapter == NULL || driver == NULL || adapter->driver != NULL || adapter->state == AULA_SIP_ADAPTER_DESTROYED) return AULA_STATUS_INVALID_ARGUMENT;
  adapter->driver = driver;
  status = aula_sip_driver_bind(driver, &adapter->endpoint, aula_sip_adapter_driver_sink, adapter);
  if (status == AULA_STATUS_UNSUPPORTED) return AULA_STATUS_OK;
  if (status != AULA_STATUS_OK) { adapter->driver = NULL; return status; }
  adapter->state = AULA_SIP_ADAPTER_ACTIVE;
  return AULA_STATUS_OK;
}

aula_status aula_sip_adapter_authorize_resolved_peer(aula_sip_adapter *adapter,
                                                        const aula_sip_transport_info *peer, int is_fallback) {
  return aula_sip_authorize_peer(adapter, peer, is_fallback);
}

aula_status aula_sip_adapter_set_outbound_target(
    aula_sip_adapter *adapter, const char *target_uri) {
  aula_sip_dial_target parsed;
  if (adapter == NULL || target_uri == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (adapter->state != AULA_SIP_ADAPTER_ACTIVE || adapter->dialogs != NULL)
    return AULA_STATUS_STATE_ERROR;
  if (aula_sip_parse_dial_target(target_uri, &parsed) != AULA_STATUS_OK ||
      !aula_sip_copy(adapter->outbound_uri, sizeof(adapter->outbound_uri),
                      target_uri))
    return AULA_STATUS_INVALID_DATA;
  adapter->endpoint.outbound_uri = adapter->outbound_uri;
  adapter->unavailable_reported = 0;
  return AULA_STATUS_OK;
}

void aula_sip_adapter_clear_outbound_target(aula_sip_adapter *adapter) {
  if (adapter == NULL || adapter->dialogs != NULL) return;
  aula_sip_wipe(adapter->outbound_uri, sizeof(adapter->outbound_uri));
  adapter->endpoint.outbound_uri = adapter->outbound_uri;
}

static aula_status aula_sip_resolve_and_authorize(aula_sip_adapter *adapter, aula_deadline deadline,
                                                     aula_sip_resolver_result *result) {
  aula_sip_resolver_request request;
  aula_status status;
  (void)memset(&request, 0, sizeof(request));
  request.target_uri = adapter->outbound_uri;
  request.preferred_transport = adapter->endpoint.preferred_transport;
  request.allow_tcp_fallback = adapter->allow_tcp_fallback;
  request.enable_tls = adapter->endpoint.enable_tls;
  request.deadline = deadline;
  status = aula_sip_driver_resolve(adapter->driver, &request, result);
  if (status != AULA_STATUS_OK) return status;
  return aula_sip_authorize_peer(adapter, &result->peer, result->is_fallback);
}

static int adapter_can_start_invite(const aula_sip_adapter *adapter) {
  return adapter->state == AULA_SIP_ADAPTER_ACTIVE &&
         aula_sip_driver_is_operational(adapter->driver);
}

static int local_sdp_is_bounded(aula_bytes local_sdp) {
  return local_sdp.data != NULL && local_sdp.length != 0U &&
         local_sdp.length <= AULA_SIPD_MAX_SDP_BYTES;
}

aula_status aula_sip_adapter_start_invite(aula_sip_adapter *adapter, aula_bytes local_sdp,
                                            aula_sip_dialog **out_dialog) {
  aula_sip_dialog *dialog;
  aula_sip_invite_request request;
  aula_sip_resolver_result result;
  aula_deadline deadline;
  aula_status status;
  if (adapter == NULL || out_dialog == NULL || *out_dialog != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_dialog = NULL;
  if (aula_sip_dialog_count(adapter) >= adapter->endpoint.limits.maximum_dialog_count)
    return AULA_STATUS_LIMIT_EXCEEDED;
  if (!adapter_can_start_invite(adapter)) { aula_sip_emit_unavailable(adapter); return AULA_STATUS_UNSUPPORTED; }
  if (!local_sdp_is_bounded(local_sdp)) return AULA_STATUS_LIMIT_EXCEEDED;
  status = aula_sip_validate_sdp(local_sdp);
  if (status != AULA_STATUS_OK) return status;
  status = aula_sip_deadline_after(adapter->transaction_timeout_ms, &deadline);
  if (status != AULA_STATUS_OK) return status;
  (void)memset(&result, 0, sizeof(result));
  status = aula_sip_resolve_and_authorize(adapter, deadline, &result);
  if (status != AULA_STATUS_OK) return status;
  dialog = (aula_sip_dialog *)calloc(1U, sizeof(*dialog));
  if (dialog == NULL) return AULA_STATUS_INTERNAL_ERROR;
  dialog->adapter = adapter;
  dialog->update.identifiers = &dialog->identifiers;
  dialog->update.routes = &dialog->routes;
  dialog->identifiers.call_id = dialog->call_id;
  dialog->identifiers.local_tag = dialog->local_tag;
  dialog->identifiers.remote_tag = dialog->remote_tag;
  dialog->update.peer = result.peer;
  dialog->update.local_cseq = 1U;
  dialog->outbound_method = AULA_SIP_METHOD_INVITE;
  dialog->initial_invite_transaction.role = AULA_SIP_TRANSACTION_ROLE_CLIENT;
  dialog->initial_invite_transaction.method = AULA_SIP_METHOD_INVITE;
  dialog->initial_invite_transaction.cseq_number = dialog->update.local_cseq;
  (void)memcpy(dialog->local_offer, local_sdp.data, local_sdp.length);
  dialog->local_offer_length = local_sdp.length;
  (void)memset(&request, 0, sizeof(request));
  request.target_uri = adapter->outbound_uri;
  request.local_sdp = local_sdp;
  request.peer = result.peer;
  request.cseq_number = dialog->update.local_cseq;
  request.max_retransmissions = adapter->max_retransmissions;
  request.deadline = deadline;
  status = aula_sip_driver_start_invite(adapter->driver, &request, &dialog->driver_dialog);
  if (status != AULA_STATUS_OK || dialog->driver_dialog == NULL) { aula_sip_dialog_destroy(dialog); return status == AULA_STATUS_OK ? AULA_STATUS_INTERNAL_ERROR : status; }
  dialog->next = adapter->dialogs;
  adapter->dialogs = dialog;
  *out_dialog = dialog;
  return AULA_STATUS_OK;
}

static aula_status aula_sip_dialog_status(const aula_sip_dialog *dialog) {
  if (dialog == NULL || dialog->adapter == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (dialog->adapter->state != AULA_SIP_ADAPTER_ACTIVE || !aula_sip_driver_is_operational(dialog->adapter->driver)) return AULA_STATUS_UNSUPPORTED;
  return AULA_STATUS_OK;
}

aula_status aula_sip_adapter_send_ack(aula_sip_dialog *dialog) { aula_status s = aula_sip_dialog_status(dialog); if (s != AULA_STATUS_OK) return s; if (!aula_sip_transaction_is_valid(&dialog->last_final) || dialog->last_final.method != AULA_SIP_METHOD_INVITE || dialog->last_final.role != AULA_SIP_TRANSACTION_ROLE_CLIENT || dialog->last_final_status < 200U || dialog->last_final_status >= 300U) return AULA_STATUS_STATE_ERROR; return aula_sip_driver_send_ack(dialog->adapter->driver, dialog->driver_dialog, &dialog->last_final); }
typedef struct aula_sip_outbound_state {
  uint32_t local_cseq;
  aula_sip_method outbound_method;
  aula_sip_transaction_identity active_client_transaction;
  int cancel_pending;
} aula_sip_outbound_state;

static void aula_sip_save_outbound_state(const aula_sip_dialog *dialog,
                                          aula_sip_outbound_state *saved) {
  saved->local_cseq = dialog->update.local_cseq;
  saved->outbound_method = dialog->outbound_method;
  saved->active_client_transaction = dialog->active_client_transaction;
  saved->cancel_pending = dialog->cancel_pending;
}

static void aula_sip_restore_outbound_state(aula_sip_dialog *dialog,
                                             const aula_sip_outbound_state *saved) {
  dialog->update.local_cseq = saved->local_cseq;
  dialog->outbound_method = saved->outbound_method;
  dialog->active_client_transaction = saved->active_client_transaction;
  dialog->cancel_pending = saved->cancel_pending;
}

aula_status aula_sip_adapter_send_cancel(aula_sip_dialog *dialog) {
  aula_sip_outbound_state saved;
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  aula_sip_save_outbound_state(dialog, &saved);
  dialog->outbound_method = AULA_SIP_METHOD_CANCEL;
  (void)memset(&dialog->active_client_transaction, 0,
               sizeof(dialog->active_client_transaction));
  status = aula_sip_driver_send_cancel(dialog->adapter->driver,
                                        dialog->driver_dialog,
                                        dialog->update.local_cseq);
  if (status == AULA_STATUS_OK) {
    dialog->cancel_pending = 1;
  } else if (status != AULA_STATUS_END) {
    aula_sip_restore_outbound_state(dialog, &saved);
  }
  return status;
}

aula_status aula_sip_adapter_send_bye(aula_sip_dialog *dialog) {
  aula_sip_outbound_state saved;
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  aula_sip_save_outbound_state(dialog, &saved);
  ++dialog->update.local_cseq;
  dialog->outbound_method = AULA_SIP_METHOD_BYE;
  (void)memset(&dialog->active_client_transaction, 0,
               sizeof(dialog->active_client_transaction));
  status = aula_sip_driver_send_bye(dialog->adapter->driver,
                                     dialog->driver_dialog,
                                     dialog->update.local_cseq);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_END)
    aula_sip_restore_outbound_state(dialog, &saved);
  return status;
}

aula_status aula_sip_adapter_start_reinvite(aula_sip_dialog *dialog, aula_bytes local_sdp) {
  aula_sip_reinvite_request request;
  aula_sip_outbound_state saved;
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  if (local_sdp.data == NULL || local_sdp.length == 0U || local_sdp.length > AULA_SIPD_MAX_SDP_BYTES) return AULA_STATUS_LIMIT_EXCEEDED;
  if ((status = aula_sip_validate_sdp(local_sdp)) != AULA_STATUS_OK || (status = aula_sip_deadline_after(dialog->adapter->transaction_timeout_ms, &request.deadline)) != AULA_STATUS_OK) return status;
  aula_sip_save_outbound_state(dialog, &saved);
  ++dialog->update.local_cseq;
  dialog->outbound_method = AULA_SIP_METHOD_REINVITE;
  (void)memset(&dialog->active_client_transaction, 0, sizeof(dialog->active_client_transaction));
  request.local_sdp = local_sdp;
  request.cseq_number = dialog->update.local_cseq;
  request.max_retransmissions = dialog->adapter->max_retransmissions;
  status = aula_sip_driver_start_reinvite(dialog->adapter->driver,
                                           dialog->driver_dialog, &request);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_END)
    aula_sip_restore_outbound_state(dialog, &saved);
  return status;
}

aula_status aula_sip_adapter_answer_reinvite(aula_sip_dialog *dialog, aula_bytes answer_sdp) {
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  if ((status = aula_sip_validate_answer_sdp(answer_sdp)) != AULA_STATUS_OK) return status;
  if (!dialog->inbound_reinvite_pending) return AULA_STATUS_STATE_ERROR;
  return aula_sip_driver_answer_reinvite(dialog->adapter->driver, dialog->driver_dialog, answer_sdp, &dialog->inbound_reinvite);
}

aula_status aula_sip_adapter_reject_reinvite(aula_sip_dialog *dialog, uint16_t status_code) {
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  if (status_code < 300U || status_code > 699U) return AULA_STATUS_INVALID_ARGUMENT;
  if (!dialog->inbound_reinvite_pending) return AULA_STATUS_STATE_ERROR;
  status = aula_sip_driver_reject_reinvite(
      dialog->adapter->driver, dialog->driver_dialog, status_code,
      &dialog->inbound_reinvite);
  /* A non-2xx INVITE ACK belongs to the server transaction and may never be
   * delivered to the dialog usage. Once the driver has accepted the final
   * rejection, this request is no longer pending at the portable boundary. */
  if (status == AULA_STATUS_OK) {
    dialog->inbound_reinvite_pending = 0;
    (void)memset(&dialog->inbound_reinvite, 0,
                 sizeof(dialog->inbound_reinvite));
  }
  return status;
}

aula_status aula_sip_adapter_continue_digest(aula_sip_dialog *dialog) {
  uint8_t response_buffer[512];
  aula_mutable_bytes output = {response_buffer, sizeof(response_buffer), 0U};
  aula_sip_digest_response response;
  aula_status status = aula_sip_dialog_status(dialog);
  if (status != AULA_STATUS_OK) return status;
  if (dialog->adapter->credential_provider == NULL || dialog->challenge.nonce == NULL) return AULA_STATUS_PERMISSION_DENIED;
  if (dialog->digest_attempts >= dialog->adapter->max_digest_retries) return AULA_STATUS_LIMIT_EXCEEDED;
  status = dialog->adapter->credential_provider(dialog->adapter->credential_context, &dialog->challenge, &output);
  if (status == AULA_STATUS_OK && (output.length == 0U || output.length > output.capacity)) status = AULA_STATUS_INVALID_DATA;
  if (status == AULA_STATUS_OK) { response.challenge = &dialog->challenge; response.response.data = response_buffer; response.response.length = output.length; response.attempt = dialog->digest_attempts + 1U; status = aula_sip_driver_submit_digest(dialog->adapter->driver, dialog->driver_dialog, &response); if (status == AULA_STATUS_OK) ++dialog->digest_attempts; }
  aula_sip_wipe(response_buffer, sizeof(response_buffer));
  return status;
}

aula_status aula_sip_adapter_poll(aula_sip_adapter *adapter, aula_deadline deadline) {
  aula_status status;
  if (adapter == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (adapter->state != AULA_SIP_ADAPTER_ACTIVE || !aula_sip_driver_is_operational(adapter->driver)) { aula_sip_emit_unavailable(adapter); return AULA_STATUS_UNSUPPORTED; }
  if (deadline.monotonic_ns == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_sip_driver_poll(adapter->driver, deadline);
  return aula_sip_adapter_dispatch_picture_fast_update(adapter, status);
}

aula_status aula_sip_adapter_get_registration_status(
    const aula_sip_adapter *adapter,
    aula_sip_registration_status *out_status) {
  if (adapter == NULL || out_status == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_status, 0, sizeof(*out_status));
  if (adapter->endpoint.profile != AULA_ZOOM_PROFILE_PROXY_REGISTRATION)
    return AULA_STATUS_OK;
  if (adapter->state != AULA_SIP_ADAPTER_ACTIVE || adapter->driver == NULL)
    return AULA_STATUS_UNSUPPORTED;
  return aula_sip_driver_get_registration_status(adapter->driver, out_status);
}

aula_status aula_sip_dialog_get_update(const aula_sip_dialog *dialog, aula_sip_dialog_update *out_update) {
  if (dialog == NULL || out_update == NULL || dialog->adapter == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_update = dialog->update;
  return AULA_STATUS_OK;
}

void aula_sip_dialog_destroy(aula_sip_dialog *dialog) { if (dialog != NULL) { if (dialog->adapter != NULL) aula_sip_adapter_forget_picture_fast_update_dialog(dialog->adapter, dialog); if (dialog->adapter != NULL && dialog->retired == 0) { (void)aula_sip_driver_abort_dialog(dialog->adapter->driver, dialog->driver_dialog); aula_sip_driver_destroy_dialog(dialog->adapter->driver, dialog->driver_dialog); dialog->retired = 1; } aula_sip_unlink_dialog(dialog); aula_sip_wipe(dialog, sizeof(*dialog)); free(dialog); } }
void aula_sip_adapter_destroy(aula_sip_adapter *adapter) { if (adapter != NULL) { while (adapter->dialogs != NULL) aula_sip_dialog_destroy(adapter->dialogs); adapter->state = AULA_SIP_ADAPTER_DESTROYED; aula_sip_wipe(adapter, sizeof(*adapter)); free(adapter); } }
