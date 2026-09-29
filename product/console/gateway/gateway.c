/* The gateway's route table: the definitive map from an HTTP path and method
 * to a control-protocol opcode, a required role, and dispatch flags, plus the
 * per-request plan built from it. Route-specific validation and response
 * shaping live in the gateway_route_*.c modules this table refers to. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static const route_spec ROUTES[] = {
  {"/zoom/api/v1/device/credentials", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_DEVICE, 0U, NULL},
  {"/zoom/api/v1/device/credentials", "PUT", "SCHEMA_INVALID", "credential schema is invalid",
      0U, AULA_GATEWAY_ROLE_ADMIN, R_DEVICE | R_MUTATION | R_BODY | R_REDACT, 400U,
      gateway_schema_oem_credentials},
  {"/zoom/api/v1/device/status", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_DEVICE, 0U, NULL},
  {"/zoom/api/v1/library", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_UNAVAILABLE, 0U, NULL},
  {"/zoom/api/v1/schedule", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_UNAVAILABLE, 0U, NULL},
  {"/zoom/api/v1/device/settings", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_UNAVAILABLE, 0U, NULL},
  {"/zoom/api/v1/maintenance", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_UNAVAILABLE, 0U, NULL},
  {"/zoom/api/v1/auth/logout", "POST", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_MUTATION | R_SUPPRESS | R_LOGOUT, 0U, NULL},
  {"/zoom/api/v1/auth/session", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_SUPPRESS, 0U, NULL},
  {"/zoom/api/v1/status", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_STATUS, AULA_GATEWAY_ROLE_VIEWER, 0U, 0U, NULL},
  {"/zoom/api/v1/events", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_SUBSCRIBE, AULA_GATEWAY_ROLE_VIEWER, 0U, 0U, NULL},
  {"/zoom/api/v1/calls", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_SUPPRESS, 0U, NULL},
  {"/zoom/api/v1/calls", "POST", "SCHEMA_INVALID", "body schema is invalid",
      AULA_CONTROL_OPCODE_ORIGINATE, AULA_GATEWAY_ROLE_OPERATOR, R_MUTATION | R_BODY, 400U,
      gateway_schema_call_request},
  {"/zoom/api/v1/calls/active", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_STATUS, AULA_GATEWAY_ROLE_VIEWER, 0U, 0U, NULL},
  {"/zoom/api/v1/calls/active", "DELETE", NULL, NULL,
      AULA_CONTROL_OPCODE_HANGUP, AULA_GATEWAY_ROLE_OPERATOR, R_MUTATION, 0U, NULL},
  {"/zoom/api/v1/calls/active/dtmf", "POST", "SCHEMA_INVALID", "body schema is invalid",
      AULA_CONTROL_OPCODE_DTMF, AULA_GATEWAY_ROLE_OPERATOR, R_MUTATION | R_BODY, 400U,
      gateway_schema_dtmf_request},
  {"/zoom/api/v1/calls/active/media", "PATCH", "SCHEMA_INVALID", "body schema is invalid",
      AULA_CONTROL_OPCODE_MEDIA, AULA_GATEWAY_ROLE_OPERATOR, R_MUTATION | R_BODY, 400U,
      gateway_schema_media_request},
  {"/zoom/api/v1/directory", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_SUPPRESS, 0U, NULL},
  {"/zoom/api/v1/directory", "POST", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION | R_SUPPRESS | R_DIRECTORY | R_BODY, 0U, NULL},
  {"/zoom/api/v1/directory", "DELETE", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION | R_SUPPRESS | R_DIRECTORY | R_BODY, 0U, NULL},
  {"/zoom/api/v1/media", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_STATUS, AULA_GATEWAY_ROLE_VIEWER, 0U, 0U, NULL},
  {"/zoom/api/v1/media/preview", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_VIEWER, R_SUPPRESS, 0U, NULL},
  {"/zoom/api/v1/settings", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_SETTINGS, AULA_GATEWAY_ROLE_ADMIN, 0U, 0U, NULL},
  {"/zoom/api/v1/settings", "PATCH", "SCHEMA_INVALID", "settings schema is invalid",
      AULA_CONTROL_OPCODE_SETTINGS, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION | R_BODY, 400U,
      gateway_schema_settings_request},
  {"/zoom/api/v1/settings/credentials", "POST", "SCHEMA_INVALID", "body schema is invalid",
      AULA_CONTROL_OPCODE_REPLACE_CREDENTIAL, AULA_GATEWAY_ROLE_ADMIN,
      R_MUTATION | R_REDACT | R_BODY, 400U, gateway_schema_credentials_request},
  {"/zoom/api/v1/users", "GET", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_SUPPRESS, 0U, NULL},
  {"/zoom/api/v1/users", "POST", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION | R_SUPPRESS | R_USERS | R_BODY, 0U, NULL},
  {"/zoom/api/v1/users", "DELETE", NULL, NULL,
      0U, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION | R_SUPPRESS | R_USERS | R_BODY, 0U, NULL},
  {"/zoom/api/v1/diagnostics", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_STATUS, AULA_GATEWAY_ROLE_ADMIN, 0U, 0U, NULL},
  {"/zoom/api/v1/diagnostics/metrics", "GET", NULL, NULL,
      AULA_CONTROL_OPCODE_METRICS, AULA_GATEWAY_ROLE_ADMIN, 0U, 0U, NULL},
  {"/zoom/api/v1/diagnostics/tests", "POST", "SCHEMA_INVALID", "body must be exactly {}",
      AULA_CONTROL_OPCODE_DIAGNOSTICS, AULA_GATEWAY_ROLE_ADMIN, R_MUTATION, 400U,
      gateway_schema_diagnostics_request}
};

int gateway_dispatch_route(const aula_gateway_request *request, aula_gateway_response *response,
                           route_plan *plan) {
  size_t index;
  const route_spec *route = NULL;
  for (index = 0U; index < sizeof(ROUTES) / sizeof(ROUTES[0]); ++index)
    if (strcmp(request->path, ROUTES[index].path) == 0 && strcmp(request->method, ROUTES[index].method) == 0) {
      route = &ROUTES[index];
      break;
    }
  if (route == NULL && gateway_opaque_export_id(request->path) && strcmp(request->method, "GET") == 0) {
    plan->role = AULA_GATEWAY_ROLE_ADMIN;
    plan->opcode = AULA_CONTROL_OPCODE_STATUS;
    plan->flags = R_EXPORT;
    return 1;
  }
  if (route == NULL) return 0;
  if (route->schema != NULL && !route->schema(request->body)) {
    gateway_write_error(response, route->status, route->code, route->message);
    return -1;
  }
  plan->opcode = route->opcode;
  plan->role = route->role;
  plan->flags = route->flags;
  if ((route->flags & R_BODY) != 0U) plan->payload = request->body;
  return 1;
}

void gateway_save_mutation(aula_gateway *gateway, const aula_gateway_session *session,
                           const aula_gateway_request *request, const route_plan *plan,
                           const aula_gateway_response *response) {
  if ((plan->flags & R_MUTATION) != 0U && plan->has_request_hash)
    gateway_save_idempotency(gateway, session, request->idempotency_key, request->path,
                             plan->request_hash, response);
}

static int operation_request_id(const aula_gateway_session *session, const aula_gateway_request *request,
                                uint32_t *output) {
  EVP_MD_CTX *context = EVP_MD_CTX_new();
  unsigned int length = 0U;
  uint8_t digest[AULA_GATEWAY_HASH_BYTES];
  uint32_t value = 0U;
  int valid;
  if (context == NULL || session == NULL || request == NULL || request->path == NULL ||
      request->idempotency_key == NULL || output == NULL) {
    EVP_MD_CTX_free(context);
    return 0;
  }
  valid = EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
      EVP_DigestUpdate(context, session->session_id, sizeof(session->session_id)) == 1 &&
      EVP_DigestUpdate(context, request->path, strlen(request->path) + 1U) == 1 &&
      EVP_DigestUpdate(context, request->idempotency_key, strlen(request->idempotency_key)) == 1 &&
      EVP_DigestFinal_ex(context, digest, &length) == 1 && length == sizeof(digest);
  EVP_MD_CTX_free(context);
  if (!valid) {
    OPENSSL_cleanse(digest, sizeof(digest));
    return 0;
  }
  (void)memcpy(&value, digest, sizeof(value));
  OPENSSL_cleanse(digest, sizeof(digest));
  *output = value == 0U ? 1U : value;
  return 1;
}

int gateway_preflight(aula_gateway *gateway, aula_gateway_session *session,
                      const aula_gateway_request *request, route_plan *plan,
                      aula_gateway_response *response) {
  aula_gateway_idempotency *cached;
  if ((plan->flags & R_MUTATION) != 0U && !gateway_is_safe_idempotency_key(request->idempotency_key)) {
    gateway_write_error(response, 400U, "IDEMPOTENCY_REQUIRED", "a valid idempotency key is required");
    return 1;
  }
  if ((plan->flags & R_MUTATION) != 0U && !gateway_idempotency_request_hash(request, plan->request_hash)) {
    gateway_write_error(response, (plan->flags & R_USERS) != 0U ? 400U : 500U,
                        (plan->flags & R_USERS) != 0U ? "SCHEMA_INVALID" : "INTERNAL",
                        (plan->flags & R_USERS) != 0U ? "body schema is invalid" : "request fingerprint failed");
    return 1;
  }
  if ((plan->flags & R_MUTATION) != 0U) plan->has_request_hash = 1;
  if ((plan->flags & R_MUTATION) != 0U && !operation_request_id(session, request, &plan->control_request_id)) {
    gateway_write_error(response, 500U, "INTERNAL", "operation identifier failed");
    return 1;
  }
  if ((plan->flags & R_MUTATION) != 0U &&
      (cached = gateway_find_idempotency(gateway, session, request->idempotency_key, request->path)) != NULL) {
    if (!gateway_secure_equal(cached->request_hash, plan->request_hash, sizeof(cached->request_hash))) {
      gateway_write_error(response, 409U, "IDEMPOTENCY_CONFLICT",
                          "idempotency key was already used for a different request");
      return 1;
    }
    response->status = cached->status;
    (void)snprintf(response->content_type, sizeof(response->content_type), "application/json");
    (void)snprintf(response->body, sizeof(response->body), "%s", cached->response);
    return 1;
  }
  if ((plan->flags & R_UNAVAILABLE) == 0U) return 0;
  gateway_write_error(response, 501U, "CAPABILITY_UNAVAILABLE",
                      "this operation is not enabled in the current device build");
  gateway_save_mutation(gateway, session, request, plan, response);
  return 1;
}
