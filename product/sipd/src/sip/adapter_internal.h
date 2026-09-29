#ifndef AULA_SIPD_ADAPTER_INTERNAL_H
#define AULA_SIPD_ADAPTER_INTERNAL_H

#include "aula_sipd/sip.h"

#define AULA_SIP_MAX_URI_BYTES 512U
#define AULA_SIP_MAX_USERNAME_BYTES 128U
#define AULA_SIP_MAX_SECRET_PATH_BYTES 512U
#define AULA_SIP_MAX_RETRANSMISSIONS 10U
#define AULA_SIP_MAX_TIMEOUT_MS 120000U
#define AULA_SIP_MAX_DIGEST_RETRIES 3U
#define AULA_SIP_MAX_DIGEST_FIELD_BYTES 256U

typedef void (*aula_sip_picture_fast_update_callback)(void *context);

typedef enum aula_sip_adapter_state {
  AULA_SIP_ADAPTER_UNAVAILABLE = 0,
  AULA_SIP_ADAPTER_ACTIVE,
  AULA_SIP_ADAPTER_DESTROYED
} aula_sip_adapter_state;

struct aula_sip_adapter {
  char outbound_uri[AULA_SIP_MAX_URI_BYTES];
  char username[AULA_SIP_MAX_USERNAME_BYTES];
  char secret_path[AULA_SIP_MAX_SECRET_PATH_BYTES];
  aula_sip_endpoint_config endpoint;
  uint32_t transaction_timeout_ms;
  uint32_t max_retransmissions;
  uint32_t max_digest_retries;
  int allow_tcp_fallback;
  aula_sip_event_callback event_callback;
  void *event_context;
  aula_sip_credential_provider credential_provider;
  void *credential_context;
  aula_sip_network_scope_authorizer network_scope_authorizer;
  void *network_scope_context;
  aula_sip_picture_fast_update_callback picture_fast_update_callback;
  void *picture_fast_update_context;
  uint64_t last_picture_fast_update_ns;
  struct aula_sip_dialog *last_picture_fast_update_dialog;
  struct aula_sip_dialog *picture_fast_update_dialog;
  int picture_fast_update_pending;
  int picture_fast_update_observed;
  int picture_fast_update_timestamp_valid;
  aula_sip_driver *driver;
  struct aula_sip_dialog *dialogs;
  aula_sip_adapter_state state;
  int unavailable_reported;
};

struct aula_sip_dialog {
  aula_sip_adapter *adapter;
  struct aula_sip_dialog *next;
  void *driver_dialog;
  aula_sip_dialog_update update;
  aula_sip_dialog_identifiers identifiers;
  aula_sip_route_set routes;
  char call_id[AULA_SIP_MAX_DIALOG_ID_BYTES];
  char local_tag[AULA_SIP_MAX_DIALOG_ID_BYTES];
  char remote_tag[AULA_SIP_MAX_DIALOG_ID_BYTES];
  char route_values[AULA_SIP_MAX_ROUTE_COUNT][AULA_SIP_MAX_ROUTE_URI_BYTES];
  uint8_t local_offer[AULA_SIPD_MAX_SDP_BYTES];
  size_t local_offer_length;
  aula_sip_digest_challenge challenge;
  char realm[AULA_SIP_MAX_DIGEST_FIELD_BYTES];
  char nonce[AULA_SIP_MAX_DIGEST_FIELD_BYTES];
  char opaque[AULA_SIP_MAX_DIGEST_FIELD_BYTES];
  char algorithm[AULA_SIP_MAX_DIGEST_FIELD_BYTES];
  char qop[AULA_SIP_MAX_DIGEST_FIELD_BYTES];
  uint32_t digest_attempts;
  uint32_t digest_challenge_count;
  int digest_authority_pinned;
  aula_sip_method outbound_method;
  aula_sip_transaction_identity active_client_transaction;
  aula_sip_transaction_identity initial_invite_transaction;
  aula_sip_transaction_identity accepted_invite_transaction;
  aula_sip_transaction_identity last_provisional;
  aula_sip_transaction_identity last_final;
  aula_sip_transaction_identity inbound_reinvite;
  uint16_t last_final_status;
  int inbound_reinvite_pending;
  int cancel_pending;
  int retired;
};

int aula_sip_copy(char *destination, size_t capacity, const char *source);
int aula_sip_text_is_safe(const char *text, size_t maximum);
int aula_sip_transport_is_valid(const aula_sip_transport_info *transport);
int aula_sip_transport_matches(const aula_sip_transport_info *left,
                                const aula_sip_transport_info *right);
aula_status aula_sip_authorize_peer(aula_sip_adapter *adapter,
                                      const aula_sip_transport_info *peer,
                                      int is_fallback);
aula_sip_dialog *aula_sip_find_dialog(aula_sip_adapter *adapter,
                                        void *driver_dialog);
int aula_sip_transaction_is_valid(
    const aula_sip_transaction_identity *transaction);
aula_status aula_sip_adapter_driver_sink(
    void *context, const aula_sip_driver_event *driver_event);
void aula_sip_adapter_set_picture_fast_update_callback(
    aula_sip_adapter *adapter,
    aula_sip_picture_fast_update_callback callback, void *context);
aula_status aula_sip_adapter_note_picture_fast_update(
    aula_sip_adapter *adapter, void *native_dialog);
aula_status aula_sip_adapter_dispatch_picture_fast_update(
    aula_sip_adapter *adapter, aula_status poll_status);
void aula_sip_adapter_forget_picture_fast_update_dialog(
    aula_sip_adapter *adapter, aula_sip_dialog *dialog);

#endif
