#ifndef AULA_CONSOLE_GATEWAY_H
#define AULA_CONSOLE_GATEWAY_H

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

#define AULA_GATEWAY_API_REVISION 1U
#define AULA_GATEWAY_PBKDF2_ITERATIONS 600000U
#define AULA_GATEWAY_SALT_BYTES 16U
#define AULA_GATEWAY_HASH_BYTES 32U
#define AULA_GATEWAY_SESSION_IDLE_SECONDS (15U * 60U)
#define AULA_GATEWAY_SESSION_ABSOLUTE_SECONDS (8U * 60U * 60U)
#define AULA_GATEWAY_SESSION_ROTATE_SECONDS (15U * 60U)
#define AULA_GATEWAY_SESSION_TOKEN_GRACE_SECONDS 60U
#define AULA_GATEWAY_SESSION_GRACE_TOKENS 8U
#define AULA_GATEWAY_MAX_SESSIONS 4U
#define AULA_GATEWAY_MAX_ACCOUNTS 8U
#define AULA_GATEWAY_MAX_DIRECTORY_ENTRIES 16U
#define AULA_GATEWAY_MAX_RECENTS 16U
#define AULA_GATEWAY_MAX_EVENTS 16U
#define AULA_GATEWAY_AUTH_BUCKETS 16U
#define AULA_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS 2000U
#define AULA_GATEWAY_COLLECTION_RESPONSE_BYTES 6144U
#define AULA_GATEWAY_RESPONSE_BYTES 8192U
#define AULA_GATEWAY_PREVIEW_MAX_SECONDS (5U * 60U)

typedef enum aula_gateway_role {
  AULA_GATEWAY_ROLE_VIEWER = 1,
  AULA_GATEWAY_ROLE_OPERATOR = 2,
  AULA_GATEWAY_ROLE_ADMIN = 3
} aula_gateway_role;

typedef struct aula_gateway_account {
  int configured;
  char username[33];
  uint8_t salt[AULA_GATEWAY_SALT_BYTES];
  uint8_t password_hash[AULA_GATEWAY_HASH_BYTES];
  aula_gateway_role role;
} aula_gateway_account;

/* Persisted contact and recent-call metadata intentionally excludes every
 * credential and call-control secret. */
typedef struct aula_gateway_safe_reference {
  int used;
  char id[33];
  char name[65];
  char meeting_id[12];
  char profile[33];
  char default_layout[17];
} aula_gateway_safe_reference;

typedef struct aula_gateway_event {
  int used;
  char id[33];
  char type[33];
  char message[33];
} aula_gateway_event;

typedef struct aula_gateway_session {
  int used;
  /* Stable and opaque server-side identity for idempotency records. */
  uint8_t session_id[AULA_GATEWAY_HASH_BYTES];
  uint8_t token_hash[AULA_GATEWAY_HASH_BYTES];
  uint8_t grace_token_hashes[AULA_GATEWAY_SESSION_GRACE_TOKENS]
                            [AULA_GATEWAY_HASH_BYTES];
  uint64_t grace_token_valid_until[AULA_GATEWAY_SESSION_GRACE_TOKENS];
  size_t next_grace_token;
  uint8_t csrf[AULA_GATEWAY_HASH_BYTES];
  uint64_t created_at;
  uint64_t last_seen_at;
  uint64_t rotated_at;
  char diagnostics_export_id[33];
  uint64_t diagnostics_export_expires_at;
  /* Account identity is authoritative for session invalidation. */
  char username[33];
  aula_gateway_role role;
} aula_gateway_session;

typedef struct aula_gateway_idempotency {
  int used;
  uint8_t session_hash[AULA_GATEWAY_HASH_BYTES];
  uint8_t request_hash[AULA_GATEWAY_HASH_BYTES];
  char key[81];
  char route[64];
  unsigned int status;
  char response[1024];
} aula_gateway_idempotency;

typedef struct aula_gateway_config {
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
} aula_gateway_config;

typedef struct aula_gateway_auth_bucket {
  int used;
  char key[65];
  uint64_t last_refill_at;
  unsigned int tokens;
} aula_gateway_auth_bucket;

typedef enum aula_gateway_store_result {
  AULA_GATEWAY_STORE_NOT_COMMITTED = 0,
  AULA_GATEWAY_STORE_DURABLE = 1,
  AULA_GATEWAY_STORE_DURABILITY_UNCERTAIN = 2
} aula_gateway_store_result;

typedef struct aula_gateway {
  aula_gateway_config config;
  char policy_origin[128];
  char policy_control_socket[108];
  char policy_account_store[256];
  char policy_bootstrap_code[129];
  char policy_preview_rtsp_ipv4[16];
  uint32_t policy_sipd_uid;
  /* `account` remains the first configured account for v1 source compatibility.
   * Authentication and authorization use `accounts`. */
  aula_gateway_account account;
  aula_gateway_account accounts[AULA_GATEWAY_MAX_ACCOUNTS];
  unsigned int account_revision;
  aula_gateway_safe_reference directory[AULA_GATEWAY_MAX_DIRECTORY_ENTRIES];
  unsigned int directory_revision;
  aula_gateway_safe_reference recents[AULA_GATEWAY_MAX_RECENTS];
  aula_gateway_event events[AULA_GATEWAY_MAX_EVENTS];
  int bootstrap_disabled;
  aula_gateway_session sessions[AULA_GATEWAY_MAX_SESSIONS];
  aula_gateway_idempotency idempotency[32];
  aula_gateway_auth_bucket account_buckets[AULA_GATEWAY_AUTH_BUCKETS];
  aula_gateway_auth_bucket source_buckets[AULA_GATEWAY_AUTH_BUCKETS];
  aula_gateway_auth_bucket global_bucket;
  size_t next_idempotency;
  pthread_mutex_t state_mutex;
  /* Daemon reads and mutations use independent serialized connections. */
  pthread_mutex_t read_control_mutex;
  pthread_mutex_t control_mutex;
  int login_hash_active;
  int synchronization_ready;
} aula_gateway;

typedef struct aula_gateway_request {
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
} aula_gateway_request;

typedef struct aula_gateway_response {
  unsigned int status;
  char content_type[32];
  char set_cookie[256];
  unsigned int retry_after;
  char body[AULA_GATEWAY_RESPONSE_BYTES];
} aula_gateway_response;

typedef struct aula_gateway_preview_lease {
  uint8_t session_id[AULA_GATEWAY_HASH_BYTES];
  uint64_t expires_at;
} aula_gateway_preview_lease;

typedef int (*aula_gateway_control_fn)(void *context, uint8_t opcode,
                                        uint32_t request_id,
                                        const char *payload,
                                        char *response,
                                        size_t response_capacity);

void aula_gateway_init(aula_gateway *gateway, const aula_gateway_config *config);
int aula_gateway_set_account(aula_gateway *gateway, const char *username,
                             const char *password, aula_gateway_role role,
                             const uint8_t salt[AULA_GATEWAY_SALT_BYTES]);
const aula_gateway_account *aula_gateway_find_account(
    const aula_gateway *gateway, const char *username);
unsigned int aula_gateway_account_count(const aula_gateway *gateway);
unsigned int aula_gateway_admin_count(const aula_gateway *gateway);
int aula_gateway_upsert_account(aula_gateway *gateway, const char *username,
                                 const char *password, aula_gateway_role role);
int aula_gateway_delete_account(aula_gateway *gateway, const char *username);
void aula_gateway_revoke_account_sessions(aula_gateway *gateway,
                                           const char *username);
int aula_gateway_load_account(aula_gateway *gateway);
aula_gateway_store_result aula_gateway_store_account(
    const aula_gateway *gateway);
int aula_gateway_load_policy(aula_gateway *gateway);
int aula_gateway_verify_password(const aula_gateway_account *account,
                                  const char *password);
int aula_gateway_handle(aula_gateway *gateway,
                         const aula_gateway_request *request,
                         aula_gateway_control_fn control, void *control_context,
                         aula_gateway_response *response);
int aula_gateway_control_exchange(void *context, uint8_t opcode,
                                   uint32_t request_id, const char *payload,
                                   char *response, size_t response_capacity);
int aula_gateway_frame_validate(const uint8_t *frame, size_t frame_length,
                                 uint8_t expected_opcode, uint32_t expected_request_id);
int aula_gateway_peer_uid_matches(int descriptor, uint32_t expected_uid);
int aula_gateway_control_socket_is_safe(
    const aula_gateway_config *config);
int aula_gateway_backend_response_normalize(uint8_t opcode, const char *input,
                                             char *output, size_t output_capacity);
int aula_gateway_parse_content_length(const char *text, size_t maximum,
                                       size_t *length_out);
void aula_gateway_redact(char *destination, size_t capacity, const char *source);
int aula_gateway_preview_authorize(aula_gateway *gateway,
                                    const aula_gateway_request *request,
                                    aula_gateway_preview_lease *lease,
                                    aula_gateway_response *response);
int aula_gateway_preview_lease_valid(aula_gateway *gateway,
                                      const aula_gateway_preview_lease *lease,
                                      uint64_t now);

#endif
