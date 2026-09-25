#ifndef LS200_CONSOLE_GATEWAY_H
#define LS200_CONSOLE_GATEWAY_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

#define LS200_GATEWAY_API_REVISION 1U
#define LS200_GATEWAY_PBKDF2_ITERATIONS 600000U
#define LS200_GATEWAY_SALT_BYTES 16U
#define LS200_GATEWAY_HASH_BYTES 32U
#define LS200_GATEWAY_SESSION_IDLE_SECONDS (15U * 60U)
#define LS200_GATEWAY_SESSION_ABSOLUTE_SECONDS (8U * 60U * 60U)
#define LS200_GATEWAY_SESSION_ROTATE_SECONDS (15U * 60U)
#define LS200_GATEWAY_SESSION_TOKEN_GRACE_SECONDS 60U
#define LS200_GATEWAY_SESSION_GRACE_TOKENS 8U
#define LS200_GATEWAY_MAX_SESSIONS 4U
#define LS200_GATEWAY_MAX_ACCOUNTS 8U
#define LS200_GATEWAY_MAX_DIRECTORY_ENTRIES 16U
#define LS200_GATEWAY_MAX_RECENTS 16U
#define LS200_GATEWAY_MAX_EVENTS 16U
#define LS200_GATEWAY_AUTH_BUCKETS 16U
#define LS200_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS 2000U
#define LS200_GATEWAY_COLLECTION_RESPONSE_BYTES 6144U
#define LS200_GATEWAY_RESPONSE_BYTES 8192U
#define LS200_GATEWAY_PREVIEW_MAX_SECONDS (5U * 60U)

typedef enum ls200_gateway_role {
  LS200_GATEWAY_ROLE_VIEWER = 1,
  LS200_GATEWAY_ROLE_OPERATOR = 2,
  LS200_GATEWAY_ROLE_ADMIN = 3
} ls200_gateway_role;

typedef struct ls200_gateway_account {
  int configured;
  char username[33];
  uint8_t salt[LS200_GATEWAY_SALT_BYTES];
  uint8_t password_hash[LS200_GATEWAY_HASH_BYTES];
  ls200_gateway_role role;
} ls200_gateway_account;

/* Persisted contact and recent-call metadata intentionally excludes every
 * credential and call-control secret. */
typedef struct ls200_gateway_safe_reference {
  int used;
  char id[33];
  char name[65];
  char meeting_id[12];
  char profile[33];
  char default_layout[17];
} ls200_gateway_safe_reference;

typedef struct ls200_gateway_event {
  int used;
  char id[33];
  char type[33];
  char message[33];
} ls200_gateway_event;

typedef struct ls200_gateway_session {
  int used;
  /* Stable and opaque server-side identity for idempotency records. */
  uint8_t session_id[LS200_GATEWAY_HASH_BYTES];
  uint8_t token_hash[LS200_GATEWAY_HASH_BYTES];
  uint8_t grace_token_hashes[LS200_GATEWAY_SESSION_GRACE_TOKENS]
                            [LS200_GATEWAY_HASH_BYTES];
  uint64_t grace_token_valid_until[LS200_GATEWAY_SESSION_GRACE_TOKENS];
  size_t next_grace_token;
  uint8_t csrf[LS200_GATEWAY_HASH_BYTES];
  uint64_t created_at;
  uint64_t last_seen_at;
  uint64_t rotated_at;
  char diagnostics_export_id[33];
  uint64_t diagnostics_export_expires_at;
  /* Account identity is authoritative for session invalidation. */
  char username[33];
  ls200_gateway_role role;
} ls200_gateway_session;

typedef struct ls200_gateway_idempotency {
  int used;
  uint8_t session_hash[LS200_GATEWAY_HASH_BYTES];
  uint8_t request_hash[LS200_GATEWAY_HASH_BYTES];
  char key[81];
  char route[64];
  unsigned int status;
  char response[1024];
} ls200_gateway_idempotency;

typedef struct ls200_gateway_config {
  const char *allowed_origin;
  const char *control_socket_path;
  const char *policy_path;
  /* Fixed operator-owned path; never sourced from request, argv, or environment. */
  const char *account_store_path;
  const char *bootstrap_code;
  /* Required operator-owned identity of the connected sipd server. */
  uint32_t expected_sipd_uid;
  /* Zero selects the bounded production default. */
  unsigned int control_timeout_milliseconds;
  unsigned int max_sessions;
  int preview_enabled;
  /* Operator-owned numeric private/loopback target; never request-derived. */
  const char *preview_rtsp_ipv4;
  uint16_t preview_rtsp_port;
} ls200_gateway_config;

typedef struct ls200_gateway_auth_bucket {
  int used;
  char key[65];
  uint64_t last_refill_at;
  unsigned int tokens;
} ls200_gateway_auth_bucket;

typedef enum ls200_gateway_store_result {
  LS200_GATEWAY_STORE_NOT_COMMITTED = 0,
  LS200_GATEWAY_STORE_DURABLE = 1,
  LS200_GATEWAY_STORE_DURABILITY_UNCERTAIN = 2
} ls200_gateway_store_result;

typedef struct ls200_gateway {
  ls200_gateway_config config;
  char policy_origin[128];
  char policy_control_socket[108];
  char policy_account_store[256];
  char policy_bootstrap_code[129];
  char policy_preview_rtsp_ipv4[16];
  uint32_t policy_sipd_uid;
  /* `account` remains the first configured account for v1 source compatibility.
   * Authentication and authorization use `accounts`. */
  ls200_gateway_account account;
  ls200_gateway_account accounts[LS200_GATEWAY_MAX_ACCOUNTS];
  unsigned int account_revision;
  ls200_gateway_safe_reference directory[LS200_GATEWAY_MAX_DIRECTORY_ENTRIES];
  unsigned int directory_revision;
  ls200_gateway_safe_reference recents[LS200_GATEWAY_MAX_RECENTS];
  ls200_gateway_event events[LS200_GATEWAY_MAX_EVENTS];
  int bootstrap_disabled;
  ls200_gateway_session sessions[LS200_GATEWAY_MAX_SESSIONS];
  ls200_gateway_idempotency idempotency[32];
  ls200_gateway_auth_bucket account_buckets[LS200_GATEWAY_AUTH_BUCKETS];
  ls200_gateway_auth_bucket source_buckets[LS200_GATEWAY_AUTH_BUCKETS];
  ls200_gateway_auth_bucket global_bucket;
  size_t next_idempotency;
  pthread_mutex_t state_mutex;
  /* Daemon reads and mutations use independent serialized connections. */
  pthread_mutex_t read_control_mutex;
  pthread_mutex_t control_mutex;
  int login_hash_active;
  int synchronization_ready;
} ls200_gateway;

typedef struct ls200_gateway_request {
  const char *method;
  const char *path;
  const char *origin;
  /* Browser-generated Fetch Metadata.  A safe same-origin GET may omit
   * Origin, but only when this value is exactly "same-origin". */
  const char *fetch_site;
  const char *cookie;
  const char *csrf_token;
  const char *idempotency_key;
  /* Set only by the FastCGI adapter from its server-generated parameter. */
  const char *client_identity;
  /* Trusted listener metadata supplied by nginx, never from HTTP headers. */
  const char *server_address;
  const char *server_port;
  const char *body;
  uint64_t now;
} ls200_gateway_request;

typedef struct ls200_gateway_response {
  unsigned int status;
  char content_type[32];
  char set_cookie[256];
  unsigned int retry_after;
  char body[LS200_GATEWAY_RESPONSE_BYTES];
} ls200_gateway_response;

typedef struct ls200_gateway_preview_lease {
  uint8_t session_id[LS200_GATEWAY_HASH_BYTES];
  uint64_t expires_at;
} ls200_gateway_preview_lease;

typedef int (*ls200_gateway_control_fn)(void *context, uint8_t opcode,
                                        uint32_t request_id,
                                        const char *payload,
                                        char *response,
                                        size_t response_capacity);

void ls200_gateway_init(ls200_gateway *gateway, const ls200_gateway_config *config);
int ls200_gateway_set_account(ls200_gateway *gateway, const char *username,
                             const char *password, ls200_gateway_role role,
                             const uint8_t salt[LS200_GATEWAY_SALT_BYTES]);
const ls200_gateway_account *ls200_gateway_find_account(
    const ls200_gateway *gateway, const char *username);
unsigned int ls200_gateway_account_count(const ls200_gateway *gateway);
unsigned int ls200_gateway_admin_count(const ls200_gateway *gateway);
int ls200_gateway_upsert_account(ls200_gateway *gateway, const char *username,
                                 const char *password, ls200_gateway_role role);
int ls200_gateway_delete_account(ls200_gateway *gateway, const char *username);
void ls200_gateway_revoke_account_sessions(ls200_gateway *gateway,
                                           const char *username);
int ls200_gateway_load_account(ls200_gateway *gateway);
ls200_gateway_store_result ls200_gateway_store_account(
    const ls200_gateway *gateway);
int ls200_gateway_load_policy(ls200_gateway *gateway);
int ls200_gateway_verify_password(const ls200_gateway_account *account,
                                  const char *password);
int ls200_gateway_handle(ls200_gateway *gateway,
                         const ls200_gateway_request *request,
                         ls200_gateway_control_fn control, void *control_context,
                         ls200_gateway_response *response);
int ls200_gateway_control_exchange(void *context, uint8_t opcode,
                                   uint32_t request_id, const char *payload,
                                   char *response, size_t response_capacity);
int ls200_gateway_frame_validate(const uint8_t *frame, size_t frame_length,
                                 uint8_t expected_opcode, uint32_t expected_request_id);
int ls200_gateway_peer_uid_matches(int descriptor, uint32_t expected_uid);
int ls200_gateway_control_socket_is_safe(
    const ls200_gateway_config *config);
int ls200_gateway_backend_response_normalize(uint8_t opcode, const char *input,
                                             char *output, size_t output_capacity);
int ls200_gateway_parse_content_length(const char *text, size_t maximum,
                                       size_t *length_out);
void ls200_gateway_redact(char *destination, size_t capacity, const char *source);
int ls200_gateway_preview_authorize(ls200_gateway *gateway,
                                    const ls200_gateway_request *request,
                                    ls200_gateway_preview_lease *lease,
                                    ls200_gateway_response *response);
int ls200_gateway_preview_lease_valid(ls200_gateway *gateway,
                                      const ls200_gateway_preview_lease *lease,
                                      uint64_t now);

#endif
