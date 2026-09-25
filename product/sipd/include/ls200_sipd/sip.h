#ifndef LS200_SIPD_SIP_H
#define LS200_SIPD_SIP_H

#include "ls200_sipd/config.h"
#include "ls200_sipd/zoom.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ls200_sip_endpoint_config;

#define LS200_SIP_DRIVER_VTABLE_VERSION 2U
#define LS200_SIP_MAX_DIALOG_ID_BYTES 128U
#define LS200_SIP_MAX_ROUTE_COUNT 8U
#define LS200_SIP_MAX_ROUTE_URI_BYTES 512U

typedef enum ls200_sip_dial_kind {
  LS200_SIP_DIAL_GENERIC = 0,
  LS200_SIP_DIAL_NUMERIC,
  LS200_SIP_DIAL_NUMERIC_COMPOUND
} ls200_sip_dial_kind;

/* Parsed, non-secret shape of an outbound dial target. Raw user/host text is
 * deliberately absent so this value is safe to retain outside the adapter. */
typedef struct ls200_sip_dial_target {
  ls200_sip_dial_kind kind;
  ls200_transport transport;
  uint16_t port;
  uint32_t numeric_component_count;
  int has_explicit_port;
} ls200_sip_dial_target;

typedef enum ls200_sip_method {
  LS200_SIP_METHOD_INVITE = 0,
  LS200_SIP_METHOD_ACK,
  LS200_SIP_METHOD_CANCEL,
  LS200_SIP_METHOD_BYE,
  LS200_SIP_METHOD_REINVITE
} ls200_sip_method;

typedef enum ls200_sip_registration_state {
  LS200_SIP_REGISTRATION_DISABLED = 0,
  LS200_SIP_REGISTRATION_REGISTERING,
  LS200_SIP_REGISTRATION_REGISTERED,
  LS200_SIP_REGISTRATION_FAILED
} ls200_sip_registration_state;

/* Non-secret, bounded state suitable for the local status protocol. */
typedef struct ls200_sip_registration_status {
  ls200_sip_registration_state state;
  uint16_t status_code;
  uint32_t expires_seconds;
} ls200_sip_registration_status;

typedef enum ls200_sip_transaction_role {
  LS200_SIP_TRANSACTION_ROLE_NONE = 0,
  LS200_SIP_TRANSACTION_ROLE_CLIENT,
  LS200_SIP_TRANSACTION_ROLE_SERVER
} ls200_sip_transaction_role;

/* Opaque handle ownership stays with the attached driver. */
typedef struct ls200_sip_transaction_identity {
  void *handle;
  ls200_sip_transaction_role role;
  ls200_sip_method method;
  uint32_t cseq_number;
} ls200_sip_transaction_identity;

typedef enum ls200_sip_final_classification {
  LS200_SIP_FINAL_NONE = 0,
  LS200_SIP_FINAL_TRANSIENT,
  LS200_SIP_FINAL_PERMANENT
} ls200_sip_final_classification;

typedef enum ls200_sip_event_kind {
  LS200_SIP_EVENT_PROVISIONAL = 0,
  LS200_SIP_EVENT_FINAL_RESPONSE,
  LS200_SIP_EVENT_RESOLVED_PEER,
  LS200_SIP_EVENT_TRANSPORT_FALLBACK,
  LS200_SIP_EVENT_REMOTE_BYE,
  LS200_SIP_EVENT_REMOTE_CANCEL,
  LS200_SIP_EVENT_REINVITE,
  LS200_SIP_EVENT_REMOTE_ACK,
  LS200_SIP_EVENT_DIGEST_CHALLENGE,
  LS200_SIP_EVENT_TRANSPORT_ERROR,
  LS200_SIP_EVENT_TRANSACTION_TIMEOUT,
  /* The native invite usage ended independently of a portable transaction,
   * for example because an RFC 4028 session refresh expired. */
  LS200_SIP_EVENT_SESSION_TERMINATED
} ls200_sip_event_kind;

typedef struct ls200_sip_digest_challenge {
  const char *realm;
  const char *nonce;
  const char *opaque;
  const char *algorithm;
  const char *qop;
  int proxy_challenge;
} ls200_sip_digest_challenge;

/* Mandatory limits for the selected wire parser/transaction implementation.
 * A binding must reject input before exceeding any of these values. */
typedef struct ls200_sip_driver_limits {
  uint32_t maximum_message_bytes;
  uint32_t maximum_sdp_bytes;
  uint32_t maximum_header_count;
  uint32_t maximum_transaction_count;
  uint32_t maximum_dialog_count;
  uint32_t maximum_digest_challenges_per_dialog;
} ls200_sip_driver_limits;

typedef struct ls200_sip_reinvite {
  ls200_bytes offer_sdp;
  int requires_media_restart;
  ls200_sip_transaction_identity request_transaction;
} ls200_sip_reinvite;

typedef struct ls200_sip_response {
  uint16_t status_code;
  uint32_t cseq_number;
  ls200_sip_method cseq_method;
  ls200_bytes session_description;
  int has_sdp;
  ls200_sip_final_classification final_classification;
  uint32_t retry_after_seconds;
} ls200_sip_response;

/* The driver reports peer identity after parsing Via received/rport parameters. */
typedef struct ls200_sip_transport_info {
  ls200_transport transport;
  uint16_t local_port;
  uint16_t remote_port;
  uint16_t rport;
  uint8_t remote_address[16];
  uint8_t remote_address_length;
  uint8_t received_address[16];
  uint8_t received_address_length;
} ls200_sip_transport_info;

/* Shared structural validation for transport identities before any network
 * scope or peer-pinning policy is applied. */
int ls200_sip_transport_is_valid(const ls200_sip_transport_info *transport);
/* Default public-network policy. IPv4-mapped IPv6 addresses are normalized to
 * their IPv4 tail before restricted-range classification. */
int ls200_sip_transport_is_restricted_network(
    const ls200_sip_transport_info *transport);

typedef struct ls200_sip_dialog_identifiers {
  const char *call_id;
  const char *local_tag;
  const char *remote_tag;
} ls200_sip_dialog_identifiers;

typedef struct ls200_sip_route_set {
  const char *routes[LS200_SIP_MAX_ROUTE_COUNT];
  size_t count;
} ls200_sip_route_set;

/* Parsed dialog metadata. All pointers remain owned by the driver during callbacks. */
typedef struct ls200_sip_dialog_update {
  const ls200_sip_dialog_identifiers *identifiers;
  const ls200_sip_route_set *routes;
  uint32_t local_cseq;
  uint32_t remote_cseq;
  ls200_sip_transport_info peer;
} ls200_sip_dialog_update;

typedef struct ls200_sip_event {
  ls200_sip_event_kind kind;
  ls200_sip_transaction_identity transaction;
  ls200_sip_response response;
  ls200_sip_transport_info transport;
  const char *reason_code;
  const ls200_sip_digest_challenge *digest_challenge;
  const ls200_sip_reinvite *reinvite;
  const ls200_sip_dialog_update *dialog_update;
  int is_duplicate;
} ls200_sip_event;

typedef void (*ls200_sip_event_callback)(void *context,
                                         const ls200_sip_event *event);

typedef ls200_status (*ls200_sip_credential_provider)(
    void *context, const ls200_sip_digest_challenge *challenge,
    ls200_mutable_bytes *out_response);

typedef ls200_status (*ls200_sip_network_scope_authorizer)(
    void *context, const ls200_sip_transport_info *peer, int is_fallback);

typedef struct ls200_sip_resolver_request {
  const char *target_uri;
  ls200_transport preferred_transport;
  int allow_tcp_fallback;
  int enable_tls;
  ls200_deadline deadline;
} ls200_sip_resolver_request;

typedef struct ls200_sip_resolver_result {
  ls200_sip_transport_info peer;
  int is_fallback;
} ls200_sip_resolver_result;

typedef struct ls200_sip_invite_request {
  const char *target_uri;
  ls200_bytes local_sdp;
  ls200_sip_transport_info peer;
  uint32_t cseq_number;
  uint32_t max_retransmissions;
  ls200_deadline deadline;
} ls200_sip_invite_request;

typedef struct ls200_sip_reinvite_request {
  ls200_bytes local_sdp;
  uint32_t cseq_number;
  uint32_t max_retransmissions;
  ls200_deadline deadline;
} ls200_sip_reinvite_request;

typedef struct ls200_sip_digest_response {
  const ls200_sip_digest_challenge *challenge;
  ls200_bytes response;
  uint32_t attempt;
} ls200_sip_digest_response;

/* RESOLVED_PEER and TRANSPORT_FALLBACK are adapter-wide and may use a null
 * driver_dialog. Every other event is dialog-scoped and must carry the known,
 * non-null driver dialog returned by start_invite. */
typedef struct ls200_sip_driver_event {
  void *driver_dialog;
  ls200_sip_event event;
} ls200_sip_driver_event;

typedef ls200_status (*ls200_sip_driver_event_sink)(
    void *context, const ls200_sip_driver_event *event);

/*
 * A binding parses and transports SIP itself. It must only call the sink with
 * fully parsed data; this portable layer deliberately has no wire parser.
 */
typedef struct ls200_sip_driver_vtable {
  uint32_t version;
  size_t struct_size;
  ls200_status (*create)(void *implementation_context,
                         const struct ls200_sip_endpoint_config *endpoint,
                         ls200_sip_driver_event_sink event_sink,
                         void *event_context, void **out_instance);
  void (*destroy)(void *implementation_context, void *instance);
  ls200_status (*resolve)(void *implementation_context, void *instance,
                          const ls200_sip_resolver_request *request,
                          ls200_sip_resolver_result *out_result);
  ls200_status (*start_invite)(void *implementation_context, void *instance,
                               const ls200_sip_invite_request *request,
                               void **out_dialog);
  ls200_status (*poll)(void *implementation_context, void *instance,
                       ls200_deadline deadline);
  ls200_status (*send_ack)(void *implementation_context, void *instance,
                           void *dialog,
                           const ls200_sip_transaction_identity *transaction);
  ls200_status (*send_cancel)(void *implementation_context, void *instance,
                              void *dialog, uint32_t cseq_number);
  ls200_status (*send_bye)(void *implementation_context, void *instance,
                           void *dialog, uint32_t cseq_number);
  ls200_status (*start_reinvite)(void *implementation_context, void *instance,
                                 void *dialog,
                                 const ls200_sip_reinvite_request *request);
  ls200_status (*answer_reinvite)(void *implementation_context, void *instance,
                                  void *dialog, ls200_bytes answer_sdp,
                                  const ls200_sip_transaction_identity *request_transaction);
  ls200_status (*reject_reinvite)(void *implementation_context, void *instance,
                                  void *dialog, uint16_t status_code,
                                  const ls200_sip_transaction_identity *request_transaction);
  ls200_status (*submit_digest)(void *implementation_context, void *instance,
                                void *dialog,
                                const ls200_sip_digest_response *response);
  ls200_status (*abort_dialog)(void *implementation_context, void *instance,
                               void *dialog);
  void (*destroy_dialog)(void *implementation_context, void *instance,
                         void *dialog);
  ls200_status (*get_registration_status)(
      void *implementation_context, void *instance,
      ls200_sip_registration_status *out_status);
} ls200_sip_driver_vtable;

typedef struct ls200_sip_driver_config {
  const char *selected_library;
  const char *selected_source_revision;
  void *context;
  const ls200_sip_driver_vtable *vtable;
} ls200_sip_driver_config;

typedef struct ls200_sip_endpoint_config {
  const char *outbound_uri;
  /* Deployment-selected profile copied from the immutable call snapshot.
   * Originate requests must match it before any target construction or
   * network resolution. */
  ls200_zoom_profile profile;
  const char *auth_username;
  const char *auth_secret_file;
  uint32_t transaction_timeout_ms;
  uint32_t max_retransmissions;
  uint32_t max_reconnect_attempts;
  uint32_t max_digest_retries;
  ls200_sip_driver_limits limits;
  int allow_tcp_fallback;
  int enable_tls;
  ls200_transport preferred_transport;
  ls200_sip_credential_provider credential_provider;
  void *credential_context;
  ls200_sip_network_scope_authorizer network_scope_authorizer;
  void *network_scope_context;
  ls200_sip_event_callback event_callback;
  void *event_context;
} ls200_sip_endpoint_config;

typedef struct ls200_sip_dialog ls200_sip_dialog;
typedef struct ls200_sip_adapter ls200_sip_adapter;
typedef struct ls200_sip_driver ls200_sip_driver;

ls200_status ls200_sip_parse_dial_target(const char *uri,
                                          ls200_sip_dial_target *out_target);
/* Emits only fixed classification tokens, never user, host, meeting, passcode,
 * host-key, port, or credential material. */
ls200_status ls200_sip_format_redacted_dial_target(
    const ls200_sip_dial_target *target, ls200_mutable_bytes *output);

/* This boundary intentionally exposes no third-party SIP-library types. */
ls200_status ls200_sip_adapter_create(const ls200_sip_endpoint_config *config,
                                      ls200_sip_adapter **out_adapter);
/* Atomically applies the bounded runtime policy used by subsequent invites.
 * It never accepts endpoint text, route, credential, or certificate input. */
ls200_status ls200_sip_adapter_apply_settings(
    ls200_sip_adapter *adapter, ls200_zoom_profile profile, int tls_verified);
/* Applies the complete bounded transport policy atomically while idle.  This
 * narrow form exists so callers can restore an exact private-lab UDP/TCP
 * policy after a persistence failure; public profiles still require TLS. */
ls200_status ls200_sip_adapter_apply_policy(
    ls200_sip_adapter *adapter, ls200_zoom_profile profile, int enable_tls,
    ls200_transport preferred_transport);
ls200_status ls200_sip_driver_create(const ls200_sip_driver_config *config,
                                     ls200_sip_driver **out_driver);
/* Attach succeeds for metadata-only drivers; operations then return UNSUPPORTED. */
ls200_status ls200_sip_adapter_attach_driver(ls200_sip_adapter *adapter,
                                             ls200_sip_driver *driver);
ls200_status ls200_sip_adapter_ingest_driver_event(ls200_sip_adapter *adapter,
                                                    ls200_sip_dialog *dialog,
                                                    const ls200_sip_event *event);
ls200_status ls200_sip_adapter_authorize_resolved_peer(
    ls200_sip_adapter *adapter, const ls200_sip_transport_info *peer,
    int is_fallback);
/* Replaces only the remote dial target while the adapter has no live dialog.
 * This is the typed-origination seam: callers must construct the URI through
 * the Zoom dial builder, and the adapter validates and copies it immediately. */
ls200_status ls200_sip_adapter_set_outbound_target(
    ls200_sip_adapter *adapter, const char *target_uri);
void ls200_sip_adapter_clear_outbound_target(ls200_sip_adapter *adapter);
ls200_status ls200_sip_adapter_start_invite(ls200_sip_adapter *adapter,
                                            ls200_bytes local_sdp,
                                            ls200_sip_dialog **out_dialog);
ls200_status ls200_sip_adapter_send_ack(ls200_sip_dialog *dialog);
ls200_status ls200_sip_adapter_send_cancel(ls200_sip_dialog *dialog);
ls200_status ls200_sip_adapter_send_bye(ls200_sip_dialog *dialog);
ls200_status ls200_sip_adapter_start_reinvite(ls200_sip_dialog *dialog,
                                              ls200_bytes local_sdp);
ls200_status ls200_sip_adapter_answer_reinvite(ls200_sip_dialog *dialog,
                                               ls200_bytes answer_sdp);
ls200_status ls200_sip_adapter_reject_reinvite(ls200_sip_dialog *dialog,
                                               uint16_t status_code);
ls200_status ls200_sip_adapter_continue_digest(ls200_sip_dialog *dialog);
ls200_status ls200_sip_adapter_poll(ls200_sip_adapter *adapter,
                                    ls200_deadline deadline);
ls200_status ls200_sip_adapter_get_registration_status(
    const ls200_sip_adapter *adapter,
    ls200_sip_registration_status *out_status);
ls200_status ls200_sip_dialog_get_update(const ls200_sip_dialog *dialog,
                                         ls200_sip_dialog_update *out_update);
void ls200_sip_dialog_destroy(ls200_sip_dialog *dialog);
void ls200_sip_adapter_destroy(ls200_sip_adapter *adapter);
void ls200_sip_driver_destroy(ls200_sip_driver *driver);

#ifdef __cplusplus
}
#endif

#endif
