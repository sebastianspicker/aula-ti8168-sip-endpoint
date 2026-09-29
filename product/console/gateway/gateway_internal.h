#ifndef AULA_CONSOLE_GATEWAY_INTERNAL_H
#define AULA_CONSOLE_GATEWAY_INTERNAL_H

/* Internal interface shared by the gateway's translation units.  Every
 * declaration here crosses a module boundary; anything that stays inside a
 * single module stays `static` in that module's own .c file. Nothing outside
 * the gateway may include this header. */

#include "gateway.h"

#include "aula_sipd/control_protocol.h"

#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifdef O_NOFOLLOW
#define AULA_O_NOFOLLOW O_NOFOLLOW
#else
#define AULA_O_NOFOLLOW 0
#endif

/* The gateway enforces a stricter payload cap than the daemon's wire limit;
 * the frame layout, magic, version and opcodes come from the shared header. */
#define GATEWAY_LSZ1_MAX_PAYLOAD 2048U
_Static_assert(GATEWAY_LSZ1_MAX_PAYLOAD <= AULA_CONTROL_MAX_PAYLOAD_BYTES,
              "gateway payload cap must not exceed the daemon's control wire limit");

/* ---- gateway_core.c: crypto, encoding, JSON, and filesystem primitives ---- */

int gateway_state_lock(aula_gateway *gateway);
void gateway_state_unlock(aula_gateway *gateway);
void gateway_install_secure_json_allocator(void);
void gateway_secure_json_free(void *memory);
int gateway_is_safe_username(const char *value);
int gateway_is_safe_idempotency_key(const char *value);
int gateway_secure_equal(const void *left, const void *right, size_t length);
int gateway_hex_encode(const uint8_t *input, size_t length, char *output, size_t capacity);
int gateway_hex_decode_32(const char *input, uint8_t output[AULA_GATEWAY_HASH_BYTES]);
int gateway_hex_decode(const char *input, uint8_t *output, size_t output_length);
int gateway_sha256(const uint8_t *input, size_t input_length, uint8_t output[AULA_GATEWAY_HASH_BYTES]);
int gateway_password_hash(const char *password, const uint8_t salt[AULA_GATEWAY_SALT_BYTES],
                          uint8_t output[AULA_GATEWAY_HASH_BYTES]);
int gateway_password_hash_candidate(const char *password, const uint8_t salt[AULA_GATEWAY_SALT_BYTES],
                                    uint8_t output[AULA_GATEWAY_HASH_BYTES]);
void gateway_write_error(aula_gateway_response *response, unsigned int status,
                         const char *code, const char *message);
void gateway_write_rate_limited(aula_gateway_response *response, unsigned int retry_after);
void gateway_write_success(aula_gateway_response *response, unsigned int status, const char *data);
int gateway_json_object_exact(json_t *object, const char *const *keys, size_t key_count);
json_t *gateway_parse_json(const char *body, json_error_t *error);
int gateway_safe_absolute_path(const char *path);
int gateway_open_parent_directory(const char *path, int *parent_out, char name[NAME_MAX + 1U]);
int gateway_open_verified_regular(const char *path, int missing_is_ok, int *descriptor_out);
int gateway_sync_account_file(int descriptor);
int gateway_sync_parent_directory(int descriptor);

/* ---- gateway_account.c: account identity, auth budget, and policy ---- */

int gateway_auth_budget_take(aula_gateway *gateway, const char *account_key,
                             const aula_gateway_request *request, unsigned int *retry_after);
void gateway_disable_bootstrap_code(aula_gateway *gateway);
void gateway_sync_legacy_account(aula_gateway *gateway);
int gateway_safe_reference_id(const char *value);
int gateway_safe_reference_name(const char *value);
int gateway_safe_reference_meeting_id(const char *value);
int gateway_safe_reference_profile(const char *value);
int gateway_safe_reference_layout(const char *value);

/* ---- gateway_account_store.c: persisted-account JSON deserialization ---- */

int gateway_apply_loaded_account_v1(aula_gateway *gateway, json_t *root);
int gateway_apply_loaded_accounts_v2(aula_gateway *gateway, json_t *root);
int gateway_apply_loaded_store_v3(aula_gateway *gateway, json_t *root);

/* ---- gateway_session.c: session lifecycle and request authentication ---- */

aula_gateway_session *gateway_new_session(aula_gateway *gateway, const char *username,
                                           aula_gateway_role role, uint64_t now,
                                           char cookie[256], char csrf[65]);
int gateway_request_origin_is_allowed(const aula_gateway *gateway,
                                      const aula_gateway_request *request);
int gateway_request_token_hash(const aula_gateway_request *request,
                               uint8_t token_hash[AULA_GATEWAY_HASH_BYTES]);
aula_gateway_session *gateway_find_session_by_token(
    aula_gateway *gateway, const uint8_t token_hash[AULA_GATEWAY_HASH_BYTES],
    uint64_t now, int *used_grace);
int gateway_session_csrf_is_valid(const aula_gateway_session *session, const char *csrf_token);
aula_gateway_session *gateway_authenticate(aula_gateway *gateway,
                                            const aula_gateway_request *request,
                                            aula_gateway_response *response,
                                            int csrf_required);
int gateway_require_role(aula_gateway_session *session, aula_gateway_role required,
                         aula_gateway_response *response);

/* ---- gateway_schema.c: generic JSON request-body schema helpers ---- */

int gateway_no_body_schema(const char *body);
int gateway_schema_keys(const char *body, const char *const *keys, size_t key_count);
int gateway_schema_enum(const char *body, const char *key, const char *const *values, size_t value_count);

/* ---- gateway_idempotency.c: per-session mutation idempotency records ---- */

aula_gateway_idempotency *gateway_find_idempotency(aula_gateway *gateway,
                                                     const aula_gateway_session *session,
                                                     const char *key, const char *route);
int gateway_idempotency_request_hash(const aula_gateway_request *request,
                                     uint8_t output[AULA_GATEWAY_HASH_BYTES]);
void gateway_save_idempotency(aula_gateway *gateway, const aula_gateway_session *session,
                              const char *key, const char *route,
                              const uint8_t request_hash[AULA_GATEWAY_HASH_BYTES],
                              const aula_gateway_response *response);

/* ---- gateway_preview_metadata.c: local preview capability projection ---- */

json_t *gateway_preview_status_data(const aula_gateway *gateway);

/* ---- gateway_settings.c / gateway_aec.c / gateway_status.c / gateway_metrics.c:
 * backend status and metrics envelope validation ---- */

int gateway_settings_backend_valid(json_t *root);
int gateway_aec_status_valid(json_t *aec);
int gateway_status_integer_range(json_t *object, const char *key, json_int_t minimum, json_int_t maximum);
int gateway_status_backend_valid(json_t *root);
json_t *gateway_media_status_data(json_t *status);
int gateway_metrics_backend_valid(json_t *root);

/* ---- route dispatch shared types ---- */

enum {
  R_MUTATION = 1U,
  R_SUPPRESS = 2U,
  R_REDACT = 4U,
  R_UNAVAILABLE = 8U,
  R_BODY = 16U,
  R_LOGOUT = 32U,
  R_USERS = 64U,
  R_DIRECTORY = 128U,
  R_EXPORT = 256U,
  R_DEVICE = 512U
};

typedef int (*schema_fn)(const char *);

typedef struct {
  const char *path;
  const char *method;
  const char *code;
  const char *message;
  uint8_t opcode;
  aula_gateway_role role;
  unsigned flags;
  unsigned status;
  schema_fn schema;
} route_spec;

typedef struct {
  const char *payload;
  uint8_t opcode;
  aula_gateway_role role;
  unsigned flags;
  int has_request_hash;
  uint32_t control_request_id;
  uint8_t request_hash[AULA_GATEWAY_HASH_BYTES];
} route_plan;

/* ---- gateway_route_schema.c: HTTP request-body schema validators ---- */

int gateway_digits_only(const char *value, int optional);
int gateway_schema_call_request(const char *body);
int gateway_schema_settings_request(const char *body);
int gateway_schema_dtmf_request(const char *body);
int gateway_schema_media_request(const char *body);
int gateway_schema_credentials_request(const char *body);
int gateway_schema_diagnostics_request(const char *body);
int gateway_schema_oem_credentials(const char *body);

/* ---- gateway_route_backend.c: backend response normalization and status shaping ---- */

int gateway_serialize_json(json_t *value, char *output, size_t capacity);
int gateway_json_value_is_safe(json_t *value);
int gateway_opaque_export_id(const char *path);
int gateway_shape_status_route(const aula_gateway *gateway, const char *path,
                               const char *input, char *output, size_t capacity);

/* ---- gateway_route_events.c: call-state event validation and history ---- */

int gateway_event_snapshot_backend_valid(json_t *root);
int gateway_record_events(aula_gateway *gateway, const char *body);
int gateway_write_events(const aula_gateway *gateway, aula_gateway_response *response);

/* ---- gateway_route_directory.c: safe directory and recent-call collections ---- */

int gateway_handle_directory_mutation(aula_gateway *gateway, const aula_gateway_request *request,
                                      aula_gateway_response *response);
int gateway_write_directory(const aula_gateway *gateway, aula_gateway_response *response);
int gateway_write_recents(const aula_gateway *gateway, aula_gateway_response *response);
int gateway_record_recent(aula_gateway *gateway, const char *body);

/* ---- gateway_route_users.c: HTTP-driven account mutation ---- */

int gateway_write_users(aula_gateway *gateway, aula_gateway_response *response);
int gateway_handle_user_mutation(aula_gateway *gateway, aula_gateway_session *session,
                                 const aula_gateway_request *request,
                                 aula_gateway_response *response, char revoked_username[33]);

/* ---- gateway_auth_routes.c: login and bootstrap ---- */

int gateway_dispatch_auth(aula_gateway *gateway, const aula_gateway_request *request,
                          aula_gateway_response *response);
int gateway_handle_login(aula_gateway *gateway, const aula_gateway_request *request,
                         aula_gateway_response *response);

/* ---- gateway.c: the route table and per-request dispatch plan ---- */

int gateway_dispatch_route(const aula_gateway_request *request, aula_gateway_response *response,
                           route_plan *plan);
int gateway_preflight(aula_gateway *gateway, aula_gateway_session *session,
                      const aula_gateway_request *request, route_plan *plan,
                      aula_gateway_response *response);
void gateway_save_mutation(aula_gateway *gateway, const aula_gateway_session *session,
                           const aula_gateway_request *request, const route_plan *plan,
                           const aula_gateway_response *response);

/* ---- gateway_route_response.c: route outcome shaping ---- */

int gateway_local_response(aula_gateway *gateway, aula_gateway_session *session,
                           const aula_gateway_request *request, const route_plan *plan,
                           aula_gateway_response *response);
int gateway_backend_control_reply(aula_gateway_control_fn control, void *context,
                                  const route_plan *plan, char backend[1024]);
int gateway_finish_backend_response(aula_gateway *gateway, const aula_gateway_session *identity,
                                    aula_gateway_session *live_session,
                                    const aula_gateway_request *request, const route_plan *plan,
                                    int control_result, const char *backend,
                                    aula_gateway_response *response);

/* ---- gateway_route_request.c: request validation and payload preparation ---- */

int gateway_valid_request(const aula_gateway *gateway, const aula_gateway_request *request,
                          const aula_gateway_response *response);
const aula_gateway_request *gateway_request_with_cleansed_body(
    const aula_gateway_request *request, aula_gateway_request *private_request,
    char request_copy[1024], aula_gateway_response *response);
int gateway_set_redacted_payload(route_plan *plan, const aula_gateway_request *request,
                                 char sensitive[1024], aula_gateway_response *response);

/* ---- gateway_transaction.c: the request transaction shared with gateway_device.c ---- */

typedef struct gateway_request_storage {
  aula_gateway_request request;
  char method[9];
  char path[256];
  char origin[128];
  char fetch_site[33];
  char cookie[1025];
  char csrf_token[65];
  char idempotency_key[81];
  char client_identity[65];
  char server_address[46];
  char server_port[6];
  char body[1025];
} gateway_request_storage;

typedef struct gateway_transaction {
  uint64_t started_at_milliseconds;
  uint64_t requested_at_seconds;
  gateway_request_storage storage;
  aula_gateway_request private_request;
  const aula_gateway_request *request;
  aula_gateway_session principal;
  route_plan plan;
  char request_copy[1024];
  char sensitive[1024];
} gateway_transaction;

/* ---- gateway_device.c: unprivileged device credential passthrough ---- */

int gateway_device_backend_reply(const gateway_transaction *transaction, char *backend, size_t capacity);
void gateway_finish_device_reply(int code, const char *backend, aula_gateway_response *response);

/* ---- gateway_control.c: LSZ1 wire transport to aula-sipd ---- */

int gateway_monotonic_milliseconds(uint64_t *milliseconds);

#endif
