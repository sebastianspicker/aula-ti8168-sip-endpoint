#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

ls200_gateway_session *gateway_new_session(ls200_gateway *gateway, const char *username,
                                           ls200_gateway_role role, uint64_t now,
                                           char cookie[256], char csrf[65]) {
  uint8_t token[LS200_GATEWAY_HASH_BYTES] = {0};
  char encoded_token[65] = {0};
  ls200_gateway_session *candidate = NULL;
  ls200_gateway_session *result = NULL;
  size_t index;
  csrf[0] = '\0';
  for (index = 0U; index < gateway->config.max_sessions; ++index) {
    if (!gateway->sessions[index].used) { candidate = &gateway->sessions[index]; break; }
    if (candidate == NULL || gateway->sessions[index].last_seen_at < candidate->last_seen_at)
      candidate = &gateway->sessions[index];
  }
  if (candidate == NULL || !gateway_is_safe_username(username) ||
      RAND_bytes(token, sizeof(token)) != 1) goto cleanup;
  OPENSSL_cleanse(candidate, sizeof(*candidate));
  if (RAND_bytes(candidate->session_id, sizeof(candidate->session_id)) != 1 ||
      RAND_bytes(candidate->csrf, sizeof(candidate->csrf)) != 1 ||
      !gateway_sha256(token, sizeof(token), candidate->token_hash) ||
      !gateway_hex_encode(token, sizeof(token), encoded_token, sizeof(encoded_token)))
    goto clear_candidate;
  candidate->used = 1;
  candidate->role = role;
  (void)snprintf(candidate->username, sizeof(candidate->username), "%s", username);
  candidate->created_at = now;
  candidate->last_seen_at = now;
  candidate->rotated_at = now;
  (void)snprintf(cookie, 256U, "ls200_session=%s; Path=/zoom/; Secure; HttpOnly; SameSite=Strict; Max-Age=%u",
                 encoded_token, LS200_GATEWAY_SESSION_IDLE_SECONDS);
  if (!gateway_hex_encode(candidate->csrf, sizeof(candidate->csrf), csrf, 65U))
    goto clear_candidate;
  result = candidate;
  goto cleanup;
clear_candidate:
  OPENSSL_cleanse(candidate, sizeof(*candidate));
  cookie[0] = '\0';
  csrf[0] = '\0';
cleanup:
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(encoded_token, sizeof(encoded_token));
  return result;
}

static int cookie_optional_whitespace(char value) {
  return value == ' ' || value == '\t';
}

static void trim_cookie_pair(const char **start, const char **end) {
  while (*start < *end && cookie_optional_whitespace(**start)) ++*start;
  while (*end > *start && cookie_optional_whitespace((*end)[-1])) --*end;
}

static int cookie_session_value(const char *start, const char *end, const char **value, size_t *value_length) {
  static const char name[] = "ls200_session";
  size_t pair_length = (size_t)(end - start);
  size_t name_length = sizeof(name) - 1U;
  if (pair_length < name_length || memcmp(start, name, name_length) != 0) return 0;
  if (pair_length == name_length || cookie_optional_whitespace(start[name_length])) return -1;
  if (start[name_length] != '=') return 0;
  *value = start + name_length + 1U;
  *value_length = (size_t)(end - *value);
  return 1;
}

static int cookie_token_is_lowercase_hex(const char *value, size_t length) {
  size_t index;
  if (length != 64U) return 0;
  for (index = 0U; index < length; ++index)
    if (!isdigit((unsigned char)value[index]) && (value[index] < 'a' || value[index] > 'f')) return 0;
  return 1;
}

static int extract_cookie_token(const char *cookie, char token[65]) {
  const char *cursor = cookie;
  const char *value = NULL;
  size_t value_length = 0U;
  unsigned int matches = 0U;
  if (cookie == NULL) return 0;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ';');
    const char *pair_end = end == NULL ? cursor + strlen(cursor) : end;
    const char *pair_start = cursor;
    int session_pair;
    trim_cookie_pair(&pair_start, &pair_end);
    session_pair = cookie_session_value(pair_start, pair_end, &value, &value_length);
    if (session_pair < 0) return 0;
    if (session_pair > 0) ++matches;
    if (end == NULL) break;
    cursor = end + 1;
  }
  if (matches != 1U || !cookie_token_is_lowercase_hex(value, value_length)) return 0;
  (void)memcpy(token, value, 64U);
  token[64] = '\0';
  return 1;
}

static int rotate_session(ls200_gateway_session *session, uint64_t now, char cookie[256]) {
  uint8_t token[LS200_GATEWAY_HASH_BYTES];
  uint8_t token_hash[LS200_GATEWAY_HASH_BYTES];
  char encoded[65];
  int rotated = 0;
  (void)memset(token, 0, sizeof(token));
  (void)memset(token_hash, 0, sizeof(token_hash));
  (void)memset(encoded, 0, sizeof(encoded));
  if (RAND_bytes(token, sizeof(token)) != 1 ||
      !gateway_sha256(token, sizeof(token), token_hash) ||
      !gateway_hex_encode(token, sizeof(token), encoded, sizeof(encoded))) goto cleanup;
  {
    size_t slot = session->next_grace_token++ % LS200_GATEWAY_SESSION_GRACE_TOKENS;
    (void)memcpy(session->grace_token_hashes[slot], session->token_hash, LS200_GATEWAY_HASH_BYTES);
    session->grace_token_valid_until[slot] = now + LS200_GATEWAY_SESSION_TOKEN_GRACE_SECONDS;
  }
  (void)memcpy(session->token_hash, token_hash, sizeof(session->token_hash));
  session->rotated_at = now;
  (void)snprintf(cookie, 256U,
                 "ls200_session=%s; Path=/zoom/; Secure; HttpOnly; SameSite=Strict; Max-Age=%u",
                 encoded, LS200_GATEWAY_SESSION_IDLE_SECONDS);
  rotated = 1;
cleanup:
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
  OPENSSL_cleanse(encoded, sizeof(encoded));
  return rotated;
}

static int parse_server_port(const char *value, unsigned int *port) {
  size_t index;
  unsigned int parsed = 0U;
  if (value == NULL || value[0] == '\0' || port == NULL ||
      strlen(value) > 5U || (value[0] == '0' && value[1] != '\0'))
    return 0;
  for (index = 0U; value[index] != '\0'; ++index) {
    if (value[index] < '0' || value[index] > '9') return 0;
    parsed = parsed * 10U + (unsigned int)(value[index] - '0');
  }
  if (parsed == 0U || parsed > 65535U) return 0;
  *port = parsed;
  return 1;
}

static int device_origin(const ls200_gateway_request *request, char origin[80], int *loopback) {
  struct in_addr ipv4;
  struct in6_addr ipv6;
  char address[INET6_ADDRSTRLEN];
  unsigned int port;
  int length;
  if (request == NULL || origin == NULL || loopback == NULL ||
      !parse_server_port(request->server_port, &port))
    return 0;
  if (request->server_address != NULL && inet_pton(AF_INET, request->server_address, &ipv4) == 1) {
    if (inet_ntop(AF_INET, &ipv4, address, sizeof(address)) == NULL) return 0;
    *loopback = (ntohl(ipv4.s_addr) >> 24U) == 127U;
    length = snprintf(origin, 80U, "https://%s:%u", address, port);
  } else if (request->server_address != NULL &&
             inet_pton(AF_INET6, request->server_address, &ipv6) == 1) {
    if (inet_ntop(AF_INET6, &ipv6, address, sizeof(address)) == NULL) return 0;
    *loopback = IN6_IS_ADDR_LOOPBACK(&ipv6);
    length = snprintf(origin, 80U, "https://[%s]:%u", address, port);
  } else {
    return 0;
  }
  return length > 0 && (size_t)length < 80U;
}

int gateway_request_origin_is_allowed(const ls200_gateway *gateway, const ls200_gateway_request *request) {
  char expected[80];
  char localhost[32];
  int loopback = 0;
  int localhost_length;
  if (gateway->config.allowed_origin != NULL && strcmp(gateway->config.allowed_origin, "device") == 0) {
    if (!device_origin(request, expected, &loopback)) return 0;
    if (request->origin != NULL) {
      if (strcmp(request->origin, expected) == 0) return 1;
      localhost_length = snprintf(localhost, sizeof(localhost), "https://localhost:%s", request->server_port);
      return loopback && localhost_length > 0 && (size_t)localhost_length < sizeof(localhost) &&
          strcmp(request->origin, localhost) == 0;
    }
  } else if (request->origin != NULL) {
    return gateway->config.allowed_origin != NULL &&
        strcmp(request->origin, gateway->config.allowed_origin) == 0;
  }
  return request->method != NULL && strcmp(request->method, "GET") == 0 &&
      request->fetch_site != NULL && strcmp(request->fetch_site, "same-origin") == 0;
}

int gateway_request_token_hash(const ls200_gateway_request *request,
                               uint8_t token_hash[LS200_GATEWAY_HASH_BYTES]) {
  uint8_t token[LS200_GATEWAY_HASH_BYTES];
  char encoded[65];
  int valid = extract_cookie_token(request->cookie, encoded) &&
      gateway_hex_decode_32(encoded, token) && gateway_sha256(token, sizeof(token), token_hash);
  OPENSSL_cleanse(token, sizeof(token));
  OPENSSL_cleanse(encoded, sizeof(encoded));
  return valid;
}

ls200_gateway_session *gateway_find_session_by_token(
    ls200_gateway *gateway, const uint8_t token_hash[LS200_GATEWAY_HASH_BYTES],
    uint64_t now, int *used_grace) {
  size_t index;
  for (index = 0U; index < gateway->config.max_sessions; ++index) {
    ls200_gateway_session *session = &gateway->sessions[index];
    if (!session->used) continue;
    if (gateway_secure_equal(session->token_hash, token_hash, LS200_GATEWAY_HASH_BYTES)) {
      *used_grace = 0;
      return session;
    }
    {
      size_t slot;
      for (slot = 0U; slot < LS200_GATEWAY_SESSION_GRACE_TOKENS; ++slot) {
        if (session->grace_token_valid_until[slot] >= now &&
            gateway_secure_equal(session->grace_token_hashes[slot], token_hash, LS200_GATEWAY_HASH_BYTES)) {
          *used_grace = 1;
          return session;
        }
      }
    }
  }
  return NULL;
}

static int session_is_expired(const ls200_gateway_session *session, uint64_t now) {
  return now < session->last_seen_at ||
      now - session->last_seen_at > LS200_GATEWAY_SESSION_IDLE_SECONDS ||
      now < session->created_at ||
      now - session->created_at > LS200_GATEWAY_SESSION_ABSOLUTE_SECONDS;
}

int gateway_session_csrf_is_valid(const ls200_gateway_session *session, const char *csrf_token) {
  uint8_t csrf[LS200_GATEWAY_HASH_BYTES] = {0};
  int valid = gateway_hex_decode_32(csrf_token, csrf) &&
      gateway_secure_equal(csrf, session->csrf, sizeof(csrf));
  OPENSSL_cleanse(csrf, sizeof(csrf));
  return valid;
}

static int refresh_session(ls200_gateway_session *session, uint64_t now, char cookie[256], int force_rotation) {
  session->last_seen_at = now;
  if (!force_rotation && (now < session->rotated_at ||
      now - session->rotated_at < LS200_GATEWAY_SESSION_ROTATE_SECONDS))
    return 1;
  return rotate_session(session, now, cookie);
}

ls200_gateway_session *gateway_authenticate(ls200_gateway *gateway, const ls200_gateway_request *request,
                                            ls200_gateway_response *response, int csrf_required) {
  uint8_t token_hash[LS200_GATEWAY_HASH_BYTES] = {0};
  ls200_gateway_session *session = NULL;
  ls200_gateway_session *result = NULL;
  int used_grace = 0;
  if (!gateway_request_origin_is_allowed(gateway, request)) {
    gateway_write_error(response, 403U, "ORIGIN_REQUIRED", "exact Origin is required");
    goto cleanup;
  }
  if (!gateway_request_token_hash(request, token_hash)) {
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    goto cleanup;
  }
  session = gateway_find_session_by_token(gateway, token_hash, request->now, &used_grace);
  if (session == NULL) {
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    goto cleanup;
  }
  if (session_is_expired(session, request->now)) {
    (void)memset(session, 0, sizeof(*session));
    gateway_write_error(response, 401U, "SESSION_EXPIRED", "session expired");
    goto cleanup;
  }
  {
    const ls200_gateway_account *account = ls200_gateway_find_account(gateway, session->username);
    if (account == NULL || account->role != session->role) {
      OPENSSL_cleanse(session, sizeof(*session));
      gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
      goto cleanup;
    }
  }
  if (csrf_required && !gateway_session_csrf_is_valid(session, request->csrf_token)) {
    gateway_write_error(response, 403U, "CSRF_INVALID", "CSRF token is invalid");
    goto cleanup;
  }
  if (!refresh_session(session, request->now, response->set_cookie, used_grace)) {
    gateway_write_error(response, 500U, "INTERNAL", "session rotation failed");
    goto cleanup;
  }
  result = session;
cleanup:
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
  return result;
}

int gateway_require_role(ls200_gateway_session *session, ls200_gateway_role required,
                         ls200_gateway_response *response) {
  if (session->role < required) {
    gateway_write_error(response, 403U, "FORBIDDEN", "role is insufficient");
    return 0;
  }
  return 1;
}

static int preview_request_is_allowed(const ls200_gateway *gateway, const ls200_gateway_request *request,
                                      ls200_gateway_response *response) {
  if (!gateway->config.preview_enabled) {
    gateway_write_error(response, 503U, "PREVIEW_UNAVAILABLE", "local preview is not configured");
    return 0;
  }
  if (request->method == NULL || request->path == NULL ||
      strcmp(request->method, "GET") != 0 ||
      strcmp(request->path, "/zoom/api/v1/media/preview.flv") != 0) {
    gateway_write_error(response, 404U, "ROUTE_NOT_FOUND", "route is not available");
    return 0;
  }
  if (!gateway_request_origin_is_allowed(gateway, request)) {
    gateway_write_error(response, 403U, "ORIGIN_REQUIRED", "exact Origin is required");
    return 0;
  }
  return 1;
}

static ls200_gateway_session *preview_session_authorize(
    ls200_gateway *gateway, const ls200_gateway_request *request, ls200_gateway_response *response) {
  uint8_t token_hash[LS200_GATEWAY_HASH_BYTES] = {0};
  ls200_gateway_session *session = NULL;
  ls200_gateway_session *result = NULL;
  const ls200_gateway_account *account;
  int used_grace = 0;
  if (!gateway_request_token_hash(request, token_hash)) {
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    goto cleanup;
  }
  session = gateway_find_session_by_token(gateway, token_hash, request->now, &used_grace);
  if (session == NULL || session_is_expired(session, request->now)) {
    if (session != NULL) OPENSSL_cleanse(session, sizeof(*session));
    gateway_write_error(response, 401U, "SESSION_EXPIRED", "session expired");
    goto cleanup;
  }
  account = ls200_gateway_find_account(gateway, session->username);
  if (account == NULL || account->role != session->role || session->role < LS200_GATEWAY_ROLE_VIEWER) {
    OPENSSL_cleanse(session, sizeof(*session));
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    goto cleanup;
  }
  if (!gateway_session_csrf_is_valid(session, request->csrf_token)) {
    gateway_write_error(response, 403U, "CSRF_INVALID", "CSRF token is invalid");
    goto cleanup;
  }
  (void)used_grace;
  result = session;
cleanup:
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
  return result;
}

int ls200_gateway_preview_authorize(ls200_gateway *gateway, const ls200_gateway_request *request,
                                    ls200_gateway_preview_lease *lease, ls200_gateway_response *response) {
  ls200_gateway_session *session;
  int result = 0;
  if (gateway == NULL || request == NULL || lease == NULL || response == NULL) return 0;
  (void)memset(response, 0, sizeof(*response));
  (void)memset(lease, 0, sizeof(*lease));
  if (!gateway_state_lock(gateway)) return 0;
  if (!preview_request_is_allowed(gateway, request, response)) goto cleanup;
  session = preview_session_authorize(gateway, request, response);
  if (session == NULL) goto cleanup;
  session->last_seen_at = request->now;
  (void)memcpy(lease->session_id, session->session_id, sizeof(lease->session_id));
  lease->expires_at = request->now + LS200_GATEWAY_PREVIEW_MAX_SECONDS;
  if (lease->expires_at < request->now ||
      lease->expires_at > session->created_at + LS200_GATEWAY_SESSION_ABSOLUTE_SECONDS)
    lease->expires_at = session->created_at + LS200_GATEWAY_SESSION_ABSOLUTE_SECONDS;
  result = 1;
cleanup:
  gateway_state_unlock(gateway);
  return result;
}

int ls200_gateway_preview_lease_valid(ls200_gateway *gateway, const ls200_gateway_preview_lease *lease,
                                      uint64_t now) {
  size_t index;
  int result = 0;
  if (gateway == NULL || lease == NULL || now == 0U || now > lease->expires_at) return 0;
  if (!gateway_state_lock(gateway)) return 0;
  for (index = 0U; index < gateway->config.max_sessions; ++index) {
    ls200_gateway_session *session = &gateway->sessions[index];
    const ls200_gateway_account *account;
    if (!session->used ||
        !gateway_secure_equal(session->session_id, lease->session_id, sizeof(session->session_id))) continue;
    account = ls200_gateway_find_account(gateway, session->username);
    if (session_is_expired(session, now) || account == NULL ||
        account->role != session->role || session->role < LS200_GATEWAY_ROLE_VIEWER)
      goto cleanup;
    session->last_seen_at = now;
    result = 1;
    break;
  }
cleanup:
  gateway_state_unlock(gateway);
  return result;
}
