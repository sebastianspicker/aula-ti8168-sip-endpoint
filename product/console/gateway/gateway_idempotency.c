#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

aula_gateway_idempotency *gateway_find_idempotency(aula_gateway *gateway,
                                                     const aula_gateway_session *session,
                                                     const char *key, const char *route) {
  size_t index;
  for (index = 0U; index < sizeof(gateway->idempotency) / sizeof(gateway->idempotency[0]); ++index) {
    aula_gateway_idempotency *entry = &gateway->idempotency[index];
    if (entry->used && strcmp(entry->key, key) == 0 && strcmp(entry->route, route) == 0 &&
        gateway_secure_equal(entry->session_hash, session->session_id, sizeof(entry->session_hash)))
      return entry;
  }
  return NULL;
}

int gateway_idempotency_request_hash(const aula_gateway_request *request,
                                     uint8_t output[AULA_GATEWAY_HASH_BYTES]) {
  const char *body;
  json_error_t error;
  json_t *root;
  char *canonical;
  size_t canonical_length;
  unsigned int output_length = 0U;
  uint8_t separator = 0U;
  EVP_MD_CTX *context;
  int valid;
  if (request == NULL || request->method == NULL || output == NULL) return 0;
  body = request->body == NULL || request->body[0] == '\0' ? "{}" : request->body;
  root = gateway_parse_json(body, &error);
  if (root == NULL) return 0;
  canonical = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
  json_decref(root);
  if (canonical == NULL) return 0;
  canonical_length = strlen(canonical);
  context = EVP_MD_CTX_new();
  valid = context != NULL &&
      EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
      EVP_DigestUpdate(context, request->method, strlen(request->method)) == 1 &&
      EVP_DigestUpdate(context, &separator, sizeof(separator)) == 1 &&
      EVP_DigestUpdate(context, canonical, canonical_length) == 1 &&
      EVP_DigestFinal_ex(context, output, &output_length) == 1 &&
      output_length == AULA_GATEWAY_HASH_BYTES;
  EVP_MD_CTX_free(context);
  gateway_secure_json_free(canonical);
  if (!valid) OPENSSL_cleanse(output, AULA_GATEWAY_HASH_BYTES);
  return valid;
}

void gateway_save_idempotency(aula_gateway *gateway, const aula_gateway_session *session,
                              const char *key, const char *route,
                              const uint8_t request_hash[AULA_GATEWAY_HASH_BYTES],
                              const aula_gateway_response *response) {
  aula_gateway_idempotency *entry =
      &gateway->idempotency[gateway->next_idempotency++ %
          (sizeof(gateway->idempotency) / sizeof(gateway->idempotency[0]))];
  (void)memset(entry, 0, sizeof(*entry));
  entry->used = 1;
  entry->status = response->status;
  (void)memcpy(entry->session_hash, session->session_id, sizeof(entry->session_hash));
  (void)memcpy(entry->request_hash, request_hash, sizeof(entry->request_hash));
  (void)snprintf(entry->key, sizeof(entry->key), "%s", key);
  (void)snprintf(entry->route, sizeof(entry->route), "%s", route);
  (void)snprintf(entry->response, sizeof(entry->response), "%s", response->body);
}
