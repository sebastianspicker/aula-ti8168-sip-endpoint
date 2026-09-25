#ifndef LS200_SIPD_ADAPTER_INTERNAL_H
#define LS200_SIPD_ADAPTER_INTERNAL_H

#include "ls200_sipd/sip.h"

#define LS200_SIP_MAX_URI_BYTES 512U
#define LS200_SIP_MAX_USERNAME_BYTES 128U
#define LS200_SIP_MAX_SECRET_PATH_BYTES 512U
#define LS200_SIP_MAX_RETRANSMISSIONS 10U
#define LS200_SIP_MAX_TIMEOUT_MS 120000U
#define LS200_SIP_MAX_DIGEST_RETRIES 3U
#define LS200_SIP_MAX_DIGEST_FIELD_BYTES 256U

typedef void (*ls200_sip_picture_fast_update_callback)(void *context);

typedef enum ls200_sip_adapter_state {
  LS200_SIP_ADAPTER_UNAVAILABLE = 0,
  LS200_SIP_ADAPTER_ACTIVE,
  LS200_SIP_ADAPTER_DESTROYED
} ls200_sip_adapter_state;

struct ls200_sip_adapter {
  char outbound_uri[LS200_SIP_MAX_URI_BYTES];
  char username[LS200_SIP_MAX_USERNAME_BYTES];
  char secret_path[LS200_SIP_MAX_SECRET_PATH_BYTES];
  ls200_sip_endpoint_config endpoint;
  uint32_t transaction_timeout_ms;
  uint32_t max_retransmissions;
  uint32_t max_digest_retries;
  int allow_tcp_fallback;
  ls200_sip_event_callback event_callback;
  void *event_context;
  ls200_sip_credential_provider credential_provider;
  void *credential_context;
  ls200_sip_network_scope_authorizer network_scope_authorizer;
  void *network_scope_context;
  ls200_sip_picture_fast_update_callback picture_fast_update_callback;
  void *picture_fast_update_context;
  uint64_t last_picture_fast_update_ns;
  struct ls200_sip_dialog *last_picture_fast_update_dialog;
  struct ls200_sip_dialog *picture_fast_update_dialog;
  int picture_fast_update_pending;
  int picture_fast_update_observed;
  int picture_fast_update_timestamp_valid;
  ls200_sip_driver *driver;
  struct ls200_sip_dialog *dialogs;
  ls200_sip_adapter_state state;
  int unavailable_reported;
};

struct ls200_sip_dialog {
  ls200_sip_adapter *adapter;
  struct ls200_sip_dialog *next;
  void *driver_dialog;
  ls200_sip_dialog_update update;
  ls200_sip_dialog_identifiers identifiers;
  ls200_sip_route_set routes;
  char call_id[LS200_SIP_MAX_DIALOG_ID_BYTES];
  char local_tag[LS200_SIP_MAX_DIALOG_ID_BYTES];
  char remote_tag[LS200_SIP_MAX_DIALOG_ID_BYTES];
  char route_values[LS200_SIP_MAX_ROUTE_COUNT][LS200_SIP_MAX_ROUTE_URI_BYTES];
  uint8_t local_offer[LS200_SIPD_MAX_SDP_BYTES];
  size_t local_offer_length;
  ls200_sip_digest_challenge challenge;
  char realm[LS200_SIP_MAX_DIGEST_FIELD_BYTES];
  char nonce[LS200_SIP_MAX_DIGEST_FIELD_BYTES];
  char opaque[LS200_SIP_MAX_DIGEST_FIELD_BYTES];
  char algorithm[LS200_SIP_MAX_DIGEST_FIELD_BYTES];
  char qop[LS200_SIP_MAX_DIGEST_FIELD_BYTES];
  uint32_t digest_attempts;
  uint32_t digest_challenge_count;
  int digest_authority_pinned;
  ls200_sip_method outbound_method;
  ls200_sip_transaction_identity active_client_transaction;
  ls200_sip_transaction_identity initial_invite_transaction;
  ls200_sip_transaction_identity accepted_invite_transaction;
  ls200_sip_transaction_identity last_provisional;
  ls200_sip_transaction_identity last_final;
  ls200_sip_transaction_identity inbound_reinvite;
  uint16_t last_final_status;
  int inbound_reinvite_pending;
  int cancel_pending;
  int retired;
};

int ls200_sip_copy(char *destination, size_t capacity, const char *source);
int ls200_sip_text_is_safe(const char *text, size_t maximum);
int ls200_sip_transport_is_valid(const ls200_sip_transport_info *transport);
int ls200_sip_transport_matches(const ls200_sip_transport_info *left,
                                const ls200_sip_transport_info *right);
ls200_status ls200_sip_authorize_peer(ls200_sip_adapter *adapter,
                                      const ls200_sip_transport_info *peer,
                                      int is_fallback);
ls200_sip_dialog *ls200_sip_find_dialog(ls200_sip_adapter *adapter,
                                        void *driver_dialog);
int ls200_sip_transaction_is_valid(
    const ls200_sip_transaction_identity *transaction);
ls200_status ls200_sip_adapter_driver_sink(
    void *context, const ls200_sip_driver_event *driver_event);
void ls200_sip_adapter_set_picture_fast_update_callback(
    ls200_sip_adapter *adapter,
    ls200_sip_picture_fast_update_callback callback, void *context);
ls200_status ls200_sip_adapter_note_picture_fast_update(
    ls200_sip_adapter *adapter, void *native_dialog);
ls200_status ls200_sip_adapter_dispatch_picture_fast_update(
    ls200_sip_adapter *adapter, ls200_status poll_status);
void ls200_sip_adapter_forget_picture_fast_update_dialog(
    ls200_sip_adapter *adapter, ls200_sip_dialog *dialog);

#endif
