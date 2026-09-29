#ifndef AULA_SIPD_SIP_H
#define AULA_SIPD_SIP_H

#include "aula_sipd/config.h"
#include "aula_sipd/zoom.h"

#ifdef __cplusplus
extern "C" {
#endif

struct aula_sip_endpoint_config;

#define AULA_SIP_DRIVER_VTABLE_VERSION 2U
#define AULA_SIP_MAX_DIALOG_ID_BYTES 128U
#define AULA_SIP_MAX_ROUTE_COUNT 8U
#define AULA_SIP_MAX_ROUTE_URI_BYTES 512U

typedef enum aula_sip_dial_kind {
  AULA_SIP_DIAL_GENERIC = 0,
  AULA_SIP_DIAL_NUMERIC,
  AULA_SIP_DIAL_NUMERIC_COMPOUND
} aula_sip_dial_kind;

/* Parsed, non-secret shape of an outbound dial target. Raw user/host text is
 * deliberately absent so this value is safe to retain outside the adapter. */
typedef struct aula_sip_dial_target {
  aula_sip_dial_kind kind;
  aula_transport transport;
  uint16_t port;
  uint32_t numeric_component_count;
  int has_explicit_port;
} aula_sip_dial_target;

typedef enum aula_sip_method {
  AULA_SIP_METHOD_INVITE = 0,
  AULA_SIP_METHOD_ACK,
  AULA_SIP_METHOD_CANCEL,
  AULA_SIP_METHOD_BYE,
  AULA_SIP_METHOD_REINVITE
} aula_sip_method;

typedef enum aula_sip_registration_state {
  AULA_SIP_REGISTRATION_DISABLED = 0,
  AULA_SIP_REGISTRATION_REGISTERING,
  AULA_SIP_REGISTRATION_REGISTERED,
  AULA_SIP_REGISTRATION_FAILED
} aula_sip_registration_state;

/* Non-secret, bounded state suitable for the local status protocol. */
typedef struct aula_sip_registration_status {
  aula_sip_registration_state state;
  uint16_t status_code;
  uint32_t expires_seconds;
} aula_sip_registration_status;

typedef enum aula_sip_transaction_role {
  AULA_SIP_TRANSACTION_ROLE_NONE = 0,
  AULA_SIP_TRANSACTION_ROLE_CLIENT,
  AULA_SIP_TRANSACTION_ROLE_SERVER
} aula_sip_transaction_role;

/* Opaque handle ownership stays with the attached driver. */
typedef struct aula_sip_transaction_identity {
  void *handle;
  aula_sip_transaction_role role;
  aula_sip_method method;
  uint32_t cseq_number;
} aula_sip_transaction_identity;

typedef enum aula_sip_final_classification {
  AULA_SIP_FINAL_NONE = 0,
  AULA_SIP_FINAL_TRANSIENT,
  AULA_SIP_FINAL_PERMANENT
} aula_sip_final_classification;

typedef enum aula_sip_event_kind {
  AULA_SIP_EVENT_PROVISIONAL = 0,
  AULA_SIP_EVENT_FINAL_RESPONSE,
  AULA_SIP_EVENT_RESOLVED_PEER,
  AULA_SIP_EVENT_TRANSPORT_FALLBACK,
  AULA_SIP_EVENT_REMOTE_BYE,
  AULA_SIP_EVENT_REMOTE_CANCEL,
  AULA_SIP_EVENT_REINVITE,
  AULA_SIP_EVENT_REMOTE_ACK,
  AULA_SIP_EVENT_DIGEST_CHALLENGE,
  AULA_SIP_EVENT_TRANSPORT_ERROR,
  AULA_SIP_EVENT_TRANSACTION_TIMEOUT,
  /* The native invite usage ended independently of a portable transaction,
   * for example because an RFC 4028 session refresh expired. */
  AULA_SIP_EVENT_SESSION_TERMINATED
} aula_sip_event_kind;

typedef struct aula_sip_digest_challenge {
  const char *realm;
  const char *nonce;
  const char *opaque;
  const char *algorithm;
  const char *qop;
  int proxy_challenge;
} aula_sip_digest_challenge;

/* Mandatory limits for the selected wire parser/transaction implementation.
 * A binding must reject input before exceeding any of these values. */
typedef struct aula_sip_driver_limits {
  uint32_t maximum_message_bytes;
  uint32_t maximum_sdp_bytes;
  uint32_t maximum_header_count;
  uint32_t maximum_transaction_count;
  uint32_t maximum_dialog_count;
  uint32_t maximum_digest_challenges_per_dialog;
} aula_sip_driver_limits;

typedef struct aula_sip_reinvite {
  aula_bytes offer_sdp;
  int requires_media_restart;
  aula_sip_transaction_identity request_transaction;
} aula_sip_reinvite;

typedef struct aula_sip_response {
  uint16_t status_code;
  uint32_t cseq_number;
  aula_sip_method cseq_method;
  aula_bytes session_description;
  int has_sdp;
  aula_sip_final_classification final_classification;
  uint32_t retry_after_seconds;
} aula_sip_response;

/* The driver reports peer identity after parsing Via received/rport parameters. */
typedef struct aula_sip_transport_info {
  aula_transport transport;
  uint16_t local_port;
  uint16_t remote_port;
  uint16_t rport;
  uint8_t remote_address[16];
  uint8_t remote_address_length;
  uint8_t received_address[16];
  uint8_t received_address_length;
} aula_sip_transport_info;

/* Shared structural validation for transport identities before any network
 * scope or peer-pinning policy is applied. */
int aula_sip_transport_is_valid(const aula_sip_transport_info *transport);
/* Default public-network policy. IPv4-mapped IPv6 addresses are normalized to
 * their IPv4 tail before restricted-range classification. */
int aula_sip_transport_is_restricted_network(
    const aula_sip_transport_info *transport);

typedef struct aula_sip_dialog_identifiers {
  const char *call_id;
  const char *local_tag;
  const char *remote_tag;
} aula_sip_dialog_identifiers;

typedef struct aula_sip_route_set {
  const char *routes[AULA_SIP_MAX_ROUTE_COUNT];
  size_t count;
} aula_sip_route_set;

/* Parsed dialog metadata. All pointers remain owned by the driver during callbacks. */
typedef struct aula_sip_dialog_update {
  const aula_sip_dialog_identifiers *identifiers;
  const aula_sip_route_set *routes;
  uint32_t local_cseq;
  uint32_t remote_cseq;
  aula_sip_transport_info peer;
} aula_sip_dialog_update;

typedef struct aula_sip_event {
  aula_sip_event_kind kind;
  aula_sip_transaction_identity transaction;
  aula_sip_response response;
  aula_sip_transport_info transport;
  const char *reason_code;
  const aula_sip_digest_challenge *digest_challenge;
  const aula_sip_reinvite *reinvite;
  const aula_sip_dialog_update *dialog_update;
  int is_duplicate;
} aula_sip_event;

typedef void (*aula_sip_event_callback)(void *context,
                                         const aula_sip_event *event);

typedef aula_status (*aula_sip_credential_provider)(
    void *context, const aula_sip_digest_challenge *challenge,
    aula_mutable_bytes *out_response);

typedef aula_status (*aula_sip_network_scope_authorizer)(
    void *context, const aula_sip_transport_info *peer, int is_fallback);

typedef struct aula_sip_resolver_request {
  const char *target_uri;
  aula_transport preferred_transport;
  int allow_tcp_fallback;
  int enable_tls;
  aula_deadline deadline;
} aula_sip_resolver_request;

typedef struct aula_sip_resolver_result {
  aula_sip_transport_info peer;
  int is_fallback;
} aula_sip_resolver_result;

typedef struct aula_sip_invite_request {
  const char *target_uri;
  aula_bytes local_sdp;
  aula_sip_transport_info peer;
  uint32_t cseq_number;
  uint32_t max_retransmissions;
  aula_deadline deadline;
} aula_sip_invite_request;

typedef struct aula_sip_reinvite_request {
  aula_bytes local_sdp;
  uint32_t cseq_number;
  uint32_t max_retransmissions;
  aula_deadline deadline;
} aula_sip_reinvite_request;

typedef struct aula_sip_digest_response {
  const aula_sip_digest_challenge *challenge;
  aula_bytes response;
  uint32_t attempt;
} aula_sip_digest_response;

/* RESOLVED_PEER and TRANSPORT_FALLBACK are adapter-wide and may use a null
 * driver_dialog. Every other event is dialog-scoped and must carry the known,
 * non-null driver dialog returned by start_invite. */
typedef struct aula_sip_driver_event {
  void *driver_dialog;
  aula_sip_event event;
} aula_sip_driver_event;

typedef aula_status (*aula_sip_driver_event_sink)(
    void *context, const aula_sip_driver_event *event);

/*
 * A binding parses and transports SIP itself. It must only call the sink with
 * fully parsed data; this portable layer deliberately has no wire parser.
 */
typedef struct aula_sip_driver_vtable {
  uint32_t version;
  size_t struct_size;
  aula_status (*create)(void *implementation_context,
                         const struct aula_sip_endpoint_config *endpoint,
                         aula_sip_driver_event_sink event_sink,
                         void *event_context, void **out_instance);
  void (*destroy)(void *implementation_context, void *instance);
  aula_status (*resolve)(void *implementation_context, void *instance,
                          const aula_sip_resolver_request *request,
                          aula_sip_resolver_result *out_result);
  aula_status (*start_invite)(void *implementation_context, void *instance,
                               const aula_sip_invite_request *request,
                               void **out_dialog);
  aula_status (*poll)(void *implementation_context, void *instance,
                       aula_deadline deadline);
  aula_status (*send_ack)(void *implementation_context, void *instance,
                           void *dialog,
                           const aula_sip_transaction_identity *transaction);
  aula_status (*send_cancel)(void *implementation_context, void *instance,
                              void *dialog, uint32_t cseq_number);
  aula_status (*send_bye)(void *implementation_context, void *instance,
                           void *dialog, uint32_t cseq_number);
  aula_status (*start_reinvite)(void *implementation_context, void *instance,
                                 void *dialog,
                                 const aula_sip_reinvite_request *request);
  aula_status (*answer_reinvite)(void *implementation_context, void *instance,
                                  void *dialog, aula_bytes answer_sdp,
                                  const aula_sip_transaction_identity *request_transaction);
  aula_status (*reject_reinvite)(void *implementation_context, void *instance,
                                  void *dialog, uint16_t status_code,
                                  const aula_sip_transaction_identity *request_transaction);
  aula_status (*submit_digest)(void *implementation_context, void *instance,
                                void *dialog,
                                const aula_sip_digest_response *response);
  aula_status (*abort_dialog)(void *implementation_context, void *instance,
                               void *dialog);
  void (*destroy_dialog)(void *implementation_context, void *instance,
                         void *dialog);
  aula_status (*get_registration_status)(
      void *implementation_context, void *instance,
      aula_sip_registration_status *out_status);
} aula_sip_driver_vtable;

typedef struct aula_sip_driver_config {
  const char *selected_library;
  const char *selected_source_revision;
  void *context;
  const aula_sip_driver_vtable *vtable;
} aula_sip_driver_config;

typedef struct aula_sip_endpoint_config {
  const char *outbound_uri;
  /* Deployment-selected profile copied from the immutable call snapshot.
   * Originate requests must match it before any target construction or
   * network resolution. */
  aula_zoom_profile profile;
  const char *auth_username;
  const char *auth_secret_file;
  uint32_t transaction_timeout_ms;
  uint32_t max_retransmissions;
  uint32_t max_reconnect_attempts;
  uint32_t max_digest_retries;
  aula_sip_driver_limits limits;
  int allow_tcp_fallback;
  int enable_tls;
  aula_transport preferred_transport;
  aula_sip_credential_provider credential_provider;
  void *credential_context;
  aula_sip_network_scope_authorizer network_scope_authorizer;
  void *network_scope_context;
  aula_sip_event_callback event_callback;
  void *event_context;
} aula_sip_endpoint_config;

typedef struct aula_sip_dialog aula_sip_dialog;
typedef struct aula_sip_adapter aula_sip_adapter;
typedef struct aula_sip_driver aula_sip_driver;

aula_status aula_sip_parse_dial_target(const char *uri,
                                          aula_sip_dial_target *out_target);
/* Emits only fixed classification tokens, never user, host, meeting, passcode,
 * host-key, port, or credential material. */
aula_status aula_sip_format_redacted_dial_target(
    const aula_sip_dial_target *target, aula_mutable_bytes *output);

/* This boundary intentionally exposes no third-party SIP-library types. */
aula_status aula_sip_adapter_create(const aula_sip_endpoint_config *config,
                                      aula_sip_adapter **out_adapter);
/* Atomically applies the bounded runtime policy used by subsequent invites.
 * It never accepts endpoint text, route, credential, or certificate input. */
aula_status aula_sip_adapter_apply_settings(
    aula_sip_adapter *adapter, aula_zoom_profile profile, int tls_verified);
/* Applies the complete bounded transport policy atomically while idle.  This
 * narrow form exists so callers can restore an exact private-lab UDP/TCP
 * policy after a persistence failure; public profiles still require TLS. */
aula_status aula_sip_adapter_apply_policy(
    aula_sip_adapter *adapter, aula_zoom_profile profile, int enable_tls,
    aula_transport preferred_transport);
aula_status aula_sip_driver_create(const aula_sip_driver_config *config,
                                     aula_sip_driver **out_driver);
/* Attach succeeds for metadata-only drivers; operations then return UNSUPPORTED. */
aula_status aula_sip_adapter_attach_driver(aula_sip_adapter *adapter,
                                             aula_sip_driver *driver);
aula_status aula_sip_adapter_ingest_driver_event(aula_sip_adapter *adapter,
                                                    aula_sip_dialog *dialog,
                                                    const aula_sip_event *event);
aula_status aula_sip_adapter_authorize_resolved_peer(
    aula_sip_adapter *adapter, const aula_sip_transport_info *peer,
    int is_fallback);
/* Replaces only the remote dial target while the adapter has no live dialog.
 * This is the typed-origination seam: callers must construct the URI through
 * the Zoom dial builder, and the adapter validates and copies it immediately. */
aula_status aula_sip_adapter_set_outbound_target(
    aula_sip_adapter *adapter, const char *target_uri);
void aula_sip_adapter_clear_outbound_target(aula_sip_adapter *adapter);
aula_status aula_sip_adapter_start_invite(aula_sip_adapter *adapter,
                                            aula_bytes local_sdp,
                                            aula_sip_dialog **out_dialog);
aula_status aula_sip_adapter_send_ack(aula_sip_dialog *dialog);
aula_status aula_sip_adapter_send_cancel(aula_sip_dialog *dialog);
aula_status aula_sip_adapter_send_bye(aula_sip_dialog *dialog);
aula_status aula_sip_adapter_start_reinvite(aula_sip_dialog *dialog,
                                              aula_bytes local_sdp);
aula_status aula_sip_adapter_answer_reinvite(aula_sip_dialog *dialog,
                                               aula_bytes answer_sdp);
aula_status aula_sip_adapter_reject_reinvite(aula_sip_dialog *dialog,
                                               uint16_t status_code);
aula_status aula_sip_adapter_continue_digest(aula_sip_dialog *dialog);
aula_status aula_sip_adapter_poll(aula_sip_adapter *adapter,
                                    aula_deadline deadline);
aula_status aula_sip_adapter_get_registration_status(
    const aula_sip_adapter *adapter,
    aula_sip_registration_status *out_status);
aula_status aula_sip_dialog_get_update(const aula_sip_dialog *dialog,
                                         aula_sip_dialog_update *out_update);
void aula_sip_dialog_destroy(aula_sip_dialog *dialog);
void aula_sip_adapter_destroy(aula_sip_adapter *adapter);
void aula_sip_driver_destroy(aula_sip_driver *driver);

#ifdef __cplusplus
}
#endif

#endif
