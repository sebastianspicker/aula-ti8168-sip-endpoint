static int client_identity_is_safe(const char *identity) {
  size_t index;
  if (identity == NULL || identity[0] == '\0' || strlen(identity) > 64U) return 0;
  for (index = 0U; identity[index] != '\0'; ++index)
    if (!isalnum((unsigned char)identity[index]) && identity[index] != '.' && identity[index] != ':' && identity[index] != '-') return 0;
  return 1;
}

static ls200_gateway_auth_bucket *find_auth_bucket(ls200_gateway_auth_bucket *buckets,
                                                    size_t bucket_count, const char *key) {
  ls200_gateway_auth_bucket *bucket = NULL;
  size_t index;
  for (index = 0U; index < bucket_count; ++index) {
    if (buckets[index].used && strcmp(buckets[index].key, key) == 0) { bucket = &buckets[index]; break; }
    if (bucket == NULL && !buckets[index].used) bucket = &buckets[index];
  }
  return bucket == NULL ? &buckets[0] : bucket;
}

static void initialize_auth_bucket(ls200_gateway_auth_bucket *bucket, const char *key,
                                   unsigned int capacity, uint64_t now) {
  if (!bucket->used || strcmp(bucket->key, key) != 0) {
    (void)memset(bucket, 0, sizeof(*bucket)); bucket->used = 1; bucket->tokens = capacity; bucket->last_refill_at = now;
    (void)snprintf(bucket->key, sizeof(bucket->key), "%s", key);
  }
}

static void refill_auth_bucket(ls200_gateway_auth_bucket *bucket, unsigned int capacity,
                               uint64_t now) {
  if (now >= bucket->last_refill_at) {
    uint64_t refill = (now - bucket->last_refill_at) / AUTH_REFILL_SECONDS;
    if (refill > 0U) {
      bucket->tokens = (unsigned int)((refill >= capacity || bucket->tokens + refill >= capacity) ? capacity : bucket->tokens + refill);
      bucket->last_refill_at += refill * AUTH_REFILL_SECONDS;
    }
  }
}

static int auth_bucket_take(ls200_gateway_auth_bucket *buckets, size_t bucket_count,
                            const char *key, unsigned int capacity, uint64_t now,
                            unsigned int *retry_after) {
  ls200_gateway_auth_bucket *bucket;
  if (now == 0U || key == NULL || strlen(key) >= sizeof(buckets[0].key)) return 0;
  bucket = find_auth_bucket(buckets, bucket_count, key);
  initialize_auth_bucket(bucket, key, capacity, now);
  refill_auth_bucket(bucket, capacity, now);
  if (bucket->tokens == 0U) { if (retry_after != NULL) *retry_after = AUTH_REFILL_SECONDS - (unsigned int)(now - bucket->last_refill_at); return 0; }
  --bucket->tokens;
  return 1;
}

static int auth_budget_take(ls200_gateway *gateway, const char *account_key,
                            const ls200_gateway_request *request, unsigned int *retry_after) {
  static const char global_key[] = "global";
  if (gateway == NULL || request == NULL || !client_identity_is_safe(request->client_identity)) return 0;
  if (!auth_bucket_take(gateway->account_buckets, LS200_GATEWAY_AUTH_BUCKETS, account_key,
                        AUTH_BUCKET_CAPACITY, request->now, retry_after) ||
      !auth_bucket_take(gateway->source_buckets, LS200_GATEWAY_AUTH_BUCKETS, request->client_identity,
                        AUTH_BUCKET_CAPACITY, request->now, retry_after) ||
      !auth_bucket_take(&gateway->global_bucket, 1U, global_key, AUTH_GLOBAL_CAPACITY, request->now, retry_after)) return 0;
  return 1;
}

void ls200_gateway_init(ls200_gateway *gateway, const ls200_gateway_config *config) {
  if (gateway == NULL) return;
  install_secure_json_allocator();
  (void)memset(gateway, 0, sizeof(*gateway));
  gateway->directory_revision = 1U;
  if (config != NULL) gateway->config = *config;
  if (gateway->config.bootstrap_code != NULL &&
      strlen(gateway->config.bootstrap_code) < sizeof(gateway->policy_bootstrap_code)) {
    (void)snprintf(gateway->policy_bootstrap_code,
                   sizeof(gateway->policy_bootstrap_code), "%s",
                   gateway->config.bootstrap_code);
    gateway->config.bootstrap_code = gateway->policy_bootstrap_code;
  } else if (gateway->config.bootstrap_code != NULL) {
    gateway->config.bootstrap_code = NULL;
  }
  if (gateway->config.control_timeout_milliseconds == 0U)
    gateway->config.control_timeout_milliseconds = LS200_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS;
  if (gateway->config.max_sessions == 0U || gateway->config.max_sessions > LS200_GATEWAY_MAX_SESSIONS)
    gateway->config.max_sessions = LS200_GATEWAY_MAX_SESSIONS;
  if (pthread_mutex_init(&gateway->state_mutex, NULL) != 0) return;
  if (pthread_mutex_init(&gateway->read_control_mutex, NULL) != 0) {
    (void)pthread_mutex_destroy(&gateway->state_mutex);
    return;
  }
  if (pthread_mutex_init(&gateway->control_mutex, NULL) != 0) {
    (void)pthread_mutex_destroy(&gateway->read_control_mutex);
    (void)pthread_mutex_destroy(&gateway->state_mutex);
    return;
  }
  gateway->synchronization_ready = 1;
}

static void disable_bootstrap_code(ls200_gateway *gateway) {
  if (gateway == NULL) return;
  OPENSSL_cleanse(gateway->policy_bootstrap_code,
                  sizeof(gateway->policy_bootstrap_code));
  gateway->config.bootstrap_code = NULL;
}

static int policy_origin_port_is_valid(const char *value) {
  unsigned int port = 0U;
  size_t index;
  if (value == NULL || value[0] == '\0' ||
      (value[0] == '0' && value[1] != '\0') || strlen(value) > 5U) return 0;
  for (index = 0U; value[index] != '\0'; ++index) {
    if (value[index] < '0' || value[index] > '9') return 0;
    port = port * 10U + (unsigned int)(value[index] - '0');
  }
  return port > 0U && port <= 65535U;
}

static int policy_origin_host_character_is_valid(unsigned char character) {
  if (character >= 'a' && character <= 'z') return 1;
  if (character >= '0' && character <= '9') return 1;
  return character == '.' || character == '-';
}

static int policy_origin_host_is_valid(const char *start, size_t length) {
  size_t index;
  if (start == NULL || length == 0U || start[0] == '.' || start[0] == '-' ||
      start[length - 1U] == '-') return 0;
  for (index = 0U; index < length; ++index) {
    unsigned char character = (unsigned char)start[index];
    if (!policy_origin_host_character_is_valid(character)) return 0;
    if (character == '.' && index + 1U < length && start[index + 1U] == '.')
      return 0;
  }
  return 1;
}

static int policy_origin_ipv6_is_valid(const char *authority) {
  struct in6_addr address;
  char host[INET6_ADDRSTRLEN];
  const char *host_end = strchr(authority, ']');
  size_t host_length;
  if (host_end == NULL || host_end == authority + 1U) return 0;
  host_length = (size_t)(host_end - authority - 1U);
  if (host_length >= sizeof(host)) return 0;
  (void)memcpy(host, authority + 1U, host_length);
  host[host_length] = '\0';
  if (inet_pton(AF_INET6, host, &address) != 1) return 0;
  if (host_end[1] == '\0') return 1;
  return host_end[1] == ':' && policy_origin_port_is_valid(host_end + 2U);
}

static int static_policy_origin_is_valid(const char *origin) {
  const char *authority;
  const char *port = NULL;
  const char *host_end;
  size_t host_length;
  if (origin == NULL || strncmp(origin, "https://", 8U) != 0) return 0;
  authority = origin + 8U;
  if (authority[0] == '\0' || strpbrk(authority, "/?#@") != NULL) return 0;
  if (authority[0] == '[') return policy_origin_ipv6_is_valid(authority);
  host_end = strchr(authority, ':');
  if (host_end != NULL) {
    if (strchr(host_end + 1U, ':') != NULL) return 0;
    port = host_end + 1U;
  } else {
    host_end = authority + strlen(authority);
  }
  host_length = (size_t)(host_end - authority);
  return policy_origin_host_is_valid(authority, host_length) &&
      (port == NULL || policy_origin_port_is_valid(port));
}

static int load_policy_origin(ls200_gateway *gateway, json_t *root) {
  json_t *value = json_object_get(root, "allowed_origin");
  const char *origin;
  if (!json_is_string(value)) return 0;
  origin = json_string_value(value);
  if (strlen(origin) >= sizeof(gateway->policy_origin) ||
      (strcmp(origin, "device") != 0 && !static_policy_origin_is_valid(origin)))
    return 0;
  (void)snprintf(gateway->policy_origin, sizeof(gateway->policy_origin), "%s", json_string_value(value));
  return 1;
}

static int load_policy_path(json_t *root, const char *key, char *destination, size_t capacity) {
  json_t *value = json_object_get(root, key);
  if (!json_is_string(value) || strlen(json_string_value(value)) >= capacity ||
      !safe_absolute_path(json_string_value(value))) return 0;
  (void)snprintf(destination, capacity, "%s", json_string_value(value));
  return 1;
}

static int load_policy_uid(ls200_gateway *gateway, json_t *root) {
  json_t *value = json_object_get(root, "expected_sipd_uid");
  if (!json_is_integer(value) || json_integer_value(value) < 0 ||
      (uint64_t)json_integer_value(value) > UINT32_MAX) return 0;
  gateway->policy_sipd_uid = (uint32_t)json_integer_value(value);
  return 1;
}

static int load_policy_control_timeout(ls200_gateway *gateway, json_t *root,
                                       int *present) {
  json_t *value = json_object_get(root, "control_timeout_milliseconds");
  *present = value != NULL;
  if (value == NULL) return 1;
  if (!json_is_integer(value) || json_integer_value(value) <= 0 ||
      (uint64_t)json_integer_value(value) > LS200_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS)
    return 0;
  gateway->config.control_timeout_milliseconds = (unsigned int)json_integer_value(value);
  return 1;
}

static int load_policy_bootstrap(ls200_gateway *gateway, json_t *root, int *present) {
  json_t *value = json_object_get(root, "bootstrap_code");
  *present = value != NULL;
  if (value == NULL) return 1;
  if (!json_is_string(value) || strlen(json_string_value(value)) < 16U ||
      strlen(json_string_value(value)) >= sizeof(gateway->policy_bootstrap_code)) return 0;
  (void)snprintf(gateway->policy_bootstrap_code, sizeof(gateway->policy_bootstrap_code), "%s", json_string_value(value));
  return 1;
}

static int preview_ipv4_is_private(const char *value) {
  struct in_addr address;
  uint32_t host;
  if (value == NULL || strlen(value) >= 16U || inet_pton(AF_INET, value, &address) != 1)
    return 0;
  host = ntohl(address.s_addr);
  return (host >> 24U) == 127U || (host >> 24U) == 10U ||
      (host >> 20U) == UINT32_C(0x0ac1) || (host >> 16U) == UINT32_C(0xc0a8);
}

static int load_policy_preview(ls200_gateway *gateway, json_t *root) {
  json_t *enabled = json_object_get(root, "preview_enabled");
  json_t *ipv4 = json_object_get(root, "preview_rtsp_ipv4");
  json_t *port = json_object_get(root, "preview_rtsp_port");
  if (!json_is_boolean(enabled) || !json_is_string(ipv4) ||
      !preview_ipv4_is_private(json_string_value(ipv4)) || !json_is_integer(port) ||
      json_integer_value(port) < 1 || json_integer_value(port) > 65535)
    return 0;
  (void)snprintf(gateway->policy_preview_rtsp_ipv4,
                 sizeof(gateway->policy_preview_rtsp_ipv4), "%s",
                 json_string_value(ipv4));
  gateway->config.preview_enabled = json_is_true(enabled);
  gateway->config.preview_rtsp_ipv4 = gateway->policy_preview_rtsp_ipv4;
  gateway->config.preview_rtsp_port = (uint16_t)json_integer_value(port);
  return 1;
}

static int load_policy_fields(ls200_gateway *gateway, json_t *root,
                              int *bootstrap_present, int *timeout_present) {
  return load_policy_origin(gateway, root) &&
      load_policy_path(root, "control_socket_path", gateway->policy_control_socket,
                       sizeof(gateway->policy_control_socket)) &&
      load_policy_path(root, "account_store_path", gateway->policy_account_store,
                       sizeof(gateway->policy_account_store)) &&
      load_policy_uid(gateway, root) &&
      load_policy_preview(gateway, root) &&
      load_policy_control_timeout(gateway, root, timeout_present) &&
      load_policy_bootstrap(gateway, root, bootstrap_present) &&
      json_object_size(root) == 7U + (*bootstrap_present ? 1U : 0U) +
          (*timeout_present ? 1U : 0U);
}

int ls200_gateway_load_policy(ls200_gateway *gateway) {
  json_error_t error;
  json_t *root;
  int descriptor;
  int bootstrap_present;
  int timeout_present;
  if (gateway == NULL || gateway->config.policy_path == NULL) return 1;
  if (open_verified_regular(gateway->config.policy_path, 0, &descriptor) != 1) {
    (void)fputs("gateway policy: verified open failed\n", stderr);
    return 0;
  }
  root = json_loadfd(descriptor, JSON_REJECT_DUPLICATES, &error);
  (void)close(descriptor);
  if (root == NULL || !json_is_object(root)) {
    (void)fputs("gateway policy: strict JSON parse failed\n", stderr);
    if (root != NULL) json_decref(root);
    return 0;
  }
  if (!load_policy_fields(gateway, root, &bootstrap_present, &timeout_present)) {
    (void)fputs("gateway policy: schema validation failed\n", stderr);
    json_decref(root);
    return 0;
  }
  json_decref(root);
  if (!bootstrap_present) disable_bootstrap_code(gateway);
  gateway->config.allowed_origin = gateway->policy_origin;
  gateway->config.control_socket_path = gateway->policy_control_socket;
  gateway->config.account_store_path = gateway->policy_account_store;
  gateway->config.expected_sipd_uid = gateway->policy_sipd_uid;
  gateway->config.bootstrap_code = gateway->policy_bootstrap_code[0] != '\0' ? gateway->policy_bootstrap_code : NULL;
  return 1;
}

int ls200_gateway_set_account(ls200_gateway *gateway, const char *username,
                             const char *password, ls200_gateway_role role,
                             const uint8_t salt[LS200_GATEWAY_SALT_BYTES]) {
  ls200_gateway_account account = {0};
  int result = 0;
  if (gateway == NULL || !is_safe_username(username) || salt == NULL ||
      role < LS200_GATEWAY_ROLE_VIEWER || role > LS200_GATEWAY_ROLE_ADMIN) return 0;
  if (!password_hash(password, salt, account.password_hash)) goto cleanup;
  account.configured = 1;
  account.role = role;
  (void)memcpy(account.salt, salt, LS200_GATEWAY_SALT_BYTES);
  (void)snprintf(account.username, sizeof(account.username), "%s", username);
  (void)memset(gateway->accounts, 0, sizeof(gateway->accounts));
  gateway->accounts[0] = account;
  gateway->account = account;
  gateway->account_revision = 1U;
  result = 1;
cleanup:
  OPENSSL_cleanse(&account, sizeof(account));
  return result;
}

static void sync_legacy_account(ls200_gateway *gateway) {
  size_t index;
  (void)memset(&gateway->account, 0, sizeof(gateway->account));
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (gateway->accounts[index].configured) {
      gateway->account = gateway->accounts[index];
      return;
    }
}

const ls200_gateway_account *ls200_gateway_find_account(
    const ls200_gateway *gateway, const char *username) {
  size_t index;
  if (gateway == NULL || !is_safe_username(username)) return NULL;
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (gateway->accounts[index].configured &&
        strcmp(gateway->accounts[index].username, username) == 0)
      return &gateway->accounts[index];
  return NULL;
}

static ls200_gateway_account *find_account_mutable(ls200_gateway *gateway,
                                                    const char *username) {
  size_t index;
  if (gateway == NULL || !is_safe_username(username)) return NULL;
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (gateway->accounts[index].configured &&
        strcmp(gateway->accounts[index].username, username) == 0)
      return &gateway->accounts[index];
  return NULL;
}

unsigned int ls200_gateway_account_count(const ls200_gateway *gateway) {
  unsigned int count = 0U;
  size_t index;
  if (gateway == NULL) return 0U;
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (gateway->accounts[index].configured) ++count;
  return count;
}

unsigned int ls200_gateway_admin_count(const ls200_gateway *gateway) {
  unsigned int count = 0U;
  size_t index;
  if (gateway == NULL) return 0U;
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (gateway->accounts[index].configured &&
        gateway->accounts[index].role == LS200_GATEWAY_ROLE_ADMIN) ++count;
  return count;
}

int ls200_gateway_upsert_account(ls200_gateway *gateway, const char *username,
                                 const char *password, ls200_gateway_role role) {
  ls200_gateway_account *account;
  ls200_gateway_account replacement = {0};
  size_t index;
  uint8_t salt[LS200_GATEWAY_SALT_BYTES] = {0};
  int result = 0;
  if (gateway == NULL || !is_safe_username(username) ||
      role < LS200_GATEWAY_ROLE_VIEWER || role > LS200_GATEWAY_ROLE_ADMIN)
    goto cleanup;
  if (RAND_bytes(salt, sizeof(salt)) != 1 ||
      !password_hash(password, salt, replacement.password_hash)) goto cleanup;
  account = find_account_mutable(gateway, username);
  if (account == NULL) {
    for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
      if (!gateway->accounts[index].configured) { account = &gateway->accounts[index]; break; }
  }
  if (account == NULL) goto cleanup;
  replacement.configured = 1;
  replacement.role = role;
  (void)memcpy(replacement.salt, salt, sizeof(salt));
  (void)snprintf(replacement.username, sizeof(replacement.username), "%s", username);
  *account = replacement;
  sync_legacy_account(gateway);
  result = 1;
cleanup:
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(&replacement, sizeof(replacement));
  return result;
}

int ls200_gateway_delete_account(ls200_gateway *gateway, const char *username) {
  ls200_gateway_account *account = find_account_mutable(gateway, username);
  if (account == NULL) return 0;
  OPENSSL_cleanse(account, sizeof(*account));
  sync_legacy_account(gateway);
  return 1;
}

void ls200_gateway_revoke_account_sessions(ls200_gateway *gateway,
                                           const char *username) {
  size_t index;
  if (gateway == NULL || username == NULL) return;
  for (index = 0U; index < gateway->config.max_sessions; ++index)
    if (gateway->sessions[index].used &&
        strcmp(gateway->sessions[index].username, username) == 0)
      OPENSSL_cleanse(&gateway->sessions[index], sizeof(gateway->sessions[index]));
}

static int safe_reference_id(const char *value) {
  size_t index;
  if (value == NULL || value[0] == '\0' || strlen(value) > 32U) return 0;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isalnum((unsigned char)value[index]) && value[index] != '_' && value[index] != '-') return 0;
  return 1;
}

static int reference_name_character_is_safe(unsigned char character) {
  return character >= 0x20U && character <= 0x7eU && character != '/' &&
      character != '@' && character != '%' && character != '\\';
}

static int reference_name_has_sip_scheme(const char *value, size_t index,
                                         size_t length) {
  return index + 3U < length && tolower((unsigned char)value[index]) == 's' &&
      tolower((unsigned char)value[index + 1U]) == 'i' &&
      tolower((unsigned char)value[index + 2U]) == 'p' && value[index + 3U] == ':';
}

static int safe_reference_name(const char *value) {
  size_t index;
  size_t length;
  if (value == NULL || value[0] == '\0' || strlen(value) > 64U) return 0;
  length = strlen(value);
  for (index = 0U; value[index] != '\0'; ++index) {
    if (!reference_name_character_is_safe((unsigned char)value[index]) ||
        reference_name_has_sip_scheme(value, index, length)) return 0;
  }
  return 1;
}

static int safe_reference_meeting_id(const char *value) {
  size_t index;
  if (value == NULL || strlen(value) < 9U || strlen(value) > 11U) return 0;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isdigit((unsigned char)value[index])) return 0;
  return 1;
}

static int safe_reference_profile(const char *value) {
  return value != NULL && (strcmp(value, "zoom_direct") == 0 ||
      strcmp(value, "zoom_proxy") == 0 || strcmp(value, "private_lab") == 0);
}

static int safe_reference_layout(const char *value) {
  return value != NULL && (strcmp(value, "gallery") == 0 ||
      strcmp(value, "full_screen") == 0 || strcmp(value, "dual_video") == 0);
}

#include "gateway_account_store.c"
