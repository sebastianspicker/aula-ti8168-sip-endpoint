#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

enum gateway_prepare_result {
  GATEWAY_PREPARE_FAILED = 0,
  GATEWAY_PREPARE_COMPLETE = 1,
  GATEWAY_PREPARE_CONTROL = 2
};

static int copy_request_field(const char *source, char *destination, size_t capacity, const char **output) {
  if (source == NULL) {
    *output = NULL;
    return 1;
  }
  if (strlen(source) >= capacity) return 0;
  (void)snprintf(destination, capacity, "%s", source);
  *output = destination;
  return 1;
}

static int copy_gateway_request(gateway_request_storage *storage, const ls200_gateway_request *request) {
  storage->request = *request;
  return copy_request_field(request->method, storage->method, sizeof(storage->method), &storage->request.method) &&
      copy_request_field(request->path, storage->path, sizeof(storage->path), &storage->request.path) &&
      copy_request_field(request->origin, storage->origin, sizeof(storage->origin), &storage->request.origin) &&
      copy_request_field(request->fetch_site, storage->fetch_site, sizeof(storage->fetch_site),
                         &storage->request.fetch_site) &&
      copy_request_field(request->cookie, storage->cookie, sizeof(storage->cookie), &storage->request.cookie) &&
      copy_request_field(request->csrf_token, storage->csrf_token, sizeof(storage->csrf_token),
                         &storage->request.csrf_token) &&
      copy_request_field(request->idempotency_key, storage->idempotency_key, sizeof(storage->idempotency_key),
                         &storage->request.idempotency_key) &&
      copy_request_field(request->client_identity, storage->client_identity, sizeof(storage->client_identity),
                         &storage->request.client_identity) &&
      copy_request_field(request->server_address, storage->server_address, sizeof(storage->server_address),
                         &storage->request.server_address) &&
      copy_request_field(request->server_port, storage->server_port, sizeof(storage->server_port),
                         &storage->request.server_port) &&
      copy_request_field(request->body, storage->body, sizeof(storage->body), &storage->request.body);
}

static void copy_gateway_principal(gateway_transaction *transaction, const ls200_gateway_session *session) {
  transaction->principal.used = 1;
  transaction->principal.role = session->role;
  (void)memcpy(transaction->principal.session_id, session->session_id,
              sizeof(transaction->principal.session_id));
  (void)memcpy(transaction->principal.token_hash, session->token_hash,
              sizeof(transaction->principal.token_hash));
  (void)snprintf(transaction->principal.username, sizeof(transaction->principal.username),
                "%s", session->username);
}

static int advance_transaction_time(gateway_transaction *transaction) {
  uint64_t now, elapsed, updated;
  if (!gateway_monotonic_milliseconds(&now) || now < transaction->started_at_milliseconds) return 0;
  elapsed = (now - transaction->started_at_milliseconds) / UINT64_C(1000);
  updated = elapsed > UINT64_MAX - transaction->requested_at_seconds ? UINT64_MAX :
      transaction->requested_at_seconds + elapsed;
  transaction->storage.request.now = updated;
  transaction->private_request.now = updated;
  return 1;
}

static ls200_gateway_session *find_principal_session(ls200_gateway *gateway,
                                                      const gateway_transaction *transaction) {
  size_t index;
  for (index = 0U; index < gateway->config.max_sessions; ++index) {
    ls200_gateway_session *session = &gateway->sessions[index];
    if (session->used && gateway_secure_equal(session->session_id, transaction->principal.session_id,
                                              sizeof(session->session_id))) return session;
  }
  return NULL;
}

static int transaction_session_is_expired(const ls200_gateway_session *session, uint64_t now) {
  return now < session->created_at ||
      now - session->created_at > LS200_GATEWAY_SESSION_ABSOLUTE_SECONDS ||
      (now >= session->last_seen_at && now - session->last_seen_at > LS200_GATEWAY_SESSION_IDLE_SECONDS);
}

static int transaction_credentials_are_valid(ls200_gateway *gateway, const gateway_transaction *transaction,
                                             const ls200_gateway_session *session) {
  uint8_t token_hash[LS200_GATEWAY_HASH_BYTES] = {0};
  int used_grace = 0;
  int valid = gateway_request_token_hash(transaction->request, token_hash) &&
      gateway_find_session_by_token(gateway, token_hash, transaction->request->now, &used_grace) == session;
  OPENSSL_cleanse(token_hash, sizeof(token_hash));
  if ((transaction->plan.flags & R_MUTATION) != 0U)
    valid = valid && gateway_session_csrf_is_valid(session, transaction->request->csrf_token);
  return valid;
}

static ls200_gateway_session *validated_principal_session(ls200_gateway *gateway,
                                                           const gateway_transaction *transaction) {
  ls200_gateway_session *session = find_principal_session(gateway, transaction);
  const ls200_gateway_account *account;
  if (session == NULL) return NULL;
  account = ls200_gateway_find_account(gateway, transaction->principal.username);
  if (transaction_session_is_expired(session, transaction->request->now) ||
      strcmp(session->username, transaction->principal.username) != 0 ||
      session->role != transaction->principal.role || account == NULL ||
      account->role != session->role || session->role < transaction->plan.role) {
    OPENSSL_cleanse(session, sizeof(*session));
    return NULL;
  }
  return transaction_credentials_are_valid(gateway, transaction, session) ? session : NULL;
}

static enum gateway_prepare_result prepare_gateway_transaction(ls200_gateway *gateway,
                                                                gateway_transaction *transaction,
                                                                ls200_gateway_response *response) {
  ls200_gateway_session *session;
  int route;
  if (gateway_dispatch_auth(gateway, transaction->request, response)) return GATEWAY_PREPARE_COMPLETE;
  route = gateway_dispatch_route(transaction->request, response, &transaction->plan);
  if (route < 0) return GATEWAY_PREPARE_COMPLETE;
  if (route == 0) {
    gateway_write_error(response, 404U, "ROUTE_NOT_FOUND", "route is not available");
    return GATEWAY_PREPARE_COMPLETE;
  }
  if (transaction->plan.payload != transaction->request->body &&
      (transaction->plan.flags & R_REDACT) == 0U && !gateway_no_body_schema(transaction->request->body)) {
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return GATEWAY_PREPARE_COMPLETE;
  }
  session = gateway_authenticate(gateway, transaction->request, response,
                                 (transaction->plan.flags & R_MUTATION) != 0U);
  if (session == NULL || !gateway_require_role(session, transaction->plan.role, response))
    return GATEWAY_PREPARE_COMPLETE;
  copy_gateway_principal(transaction, session);
  if (gateway_preflight(gateway, session, transaction->request, &transaction->plan, response) ||
      gateway_local_response(gateway, session, transaction->request, &transaction->plan, response))
    return GATEWAY_PREPARE_COMPLETE;
  if (!gateway_set_redacted_payload(&transaction->plan, transaction->request, transaction->sensitive, response))
    return GATEWAY_PREPARE_COMPLETE;
  return GATEWAY_PREPARE_CONTROL;
}

static int revalidate_gateway_transaction(ls200_gateway *gateway, gateway_transaction *transaction,
                                          ls200_gateway_response *response) {
  ls200_gateway_session *session;
  if (!advance_transaction_time(transaction)) {
    response->set_cookie[0] = '\0';
    gateway_write_error(response, 500U, "INTERNAL", "request clock is unavailable");
    return 0;
  }
  session = validated_principal_session(gateway, transaction);
  if (session == NULL) {
    response->set_cookie[0] = '\0';
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    return 0;
  }
  if (response->set_cookie[0] != '\0' &&
      !gateway_secure_equal(session->token_hash, transaction->principal.token_hash,
                            sizeof(session->token_hash)))
    response->set_cookie[0] = '\0';
  return !gateway_preflight(gateway, session, transaction->request, &transaction->plan, response);
}

static int transaction_requires_live_session(const gateway_transaction *transaction) {
  return strcmp(transaction->request->path, "/zoom/api/v1/diagnostics") == 0 ||
      (transaction->plan.flags & (R_EXPORT | R_DEVICE)) != 0U;
}

static void finish_gateway_transaction(ls200_gateway *gateway, gateway_transaction *transaction,
                                       int control_result, const char *backend,
                                       ls200_gateway_response *response) {
  ls200_gateway_session *session;
  session = advance_transaction_time(transaction) ? validated_principal_session(gateway, transaction) : NULL;
  if (session == NULL) {
    response->set_cookie[0] = '\0';
  } else if (response->set_cookie[0] != '\0' &&
             !gateway_secure_equal(session->token_hash, transaction->principal.token_hash,
                                   sizeof(session->token_hash))) {
    response->set_cookie[0] = '\0';
  }
  if (session == NULL && transaction_requires_live_session(transaction)) {
    gateway_write_error(response, 401U, "UNAUTHORIZED", "session is required");
    return;
  }
  if ((transaction->plan.flags & R_DEVICE) != 0U) {
    gateway_finish_device_reply(control_result, backend, response);
    return;
  }
  (void)gateway_finish_backend_response(gateway, &transaction->principal, session, transaction->request,
      &transaction->plan, control_result, backend, response);
}

static int execute_gateway_transaction(ls200_gateway *gateway, gateway_transaction *transaction,
                                       ls200_gateway_control_fn control, void *context,
                                       ls200_gateway_response *response) {
  char backend[1024] = {0};
  int control_result;
  pthread_mutex_t *control_lane = (transaction->plan.flags & R_MUTATION) != 0U ?
      &gateway->control_mutex : &gateway->read_control_mutex;
  if (pthread_mutex_lock(control_lane) != 0) return 0;
  if (!gateway_state_lock(gateway)) {
    (void)pthread_mutex_unlock(control_lane);
    return 0;
  }
  if (!revalidate_gateway_transaction(gateway, transaction, response)) {
    gateway_state_unlock(gateway);
    (void)pthread_mutex_unlock(control_lane);
    return 1;
  }
  gateway_state_unlock(gateway);
  control_result = (transaction->plan.flags & R_DEVICE) != 0U ?
      gateway_device_backend_reply(transaction, backend, sizeof(backend)) :
      gateway_backend_control_reply(control, context, &transaction->plan, backend);
  if (!gateway_state_lock(gateway)) {
    OPENSSL_cleanse(backend, sizeof(backend));
    (void)pthread_mutex_unlock(control_lane);
    return 0;
  }
  finish_gateway_transaction(gateway, transaction, control_result, backend, response);
  gateway_state_unlock(gateway);
  (void)pthread_mutex_unlock(control_lane);
  OPENSSL_cleanse(backend, sizeof(backend));
  return 1;
}

int ls200_gateway_handle(ls200_gateway *gateway, const ls200_gateway_request *request,
                         ls200_gateway_control_fn control, void *context, ls200_gateway_response *response) {
  gateway_transaction transaction;
  enum gateway_prepare_result prepared;
  int result = 1;
  if (!gateway_valid_request(gateway, request, response)) return 0;
  (void)memset(response, 0, sizeof(*response));
  (void)memset(&transaction, 0, sizeof(transaction));
  transaction.plan.payload = "{}";
  transaction.plan.role = LS200_GATEWAY_ROLE_VIEWER;
  if (!copy_gateway_request(&transaction.storage, request)) {
    gateway_write_error(response, 400U, "REQUEST_INVALID", "request fields exceed their bounds");
    goto cleanup;
  }
  transaction.requested_at_seconds = request->now;
  if (!gateway_monotonic_milliseconds(&transaction.started_at_milliseconds)) {
    result = 0;
    goto cleanup;
  }
  transaction.request = gateway_request_with_cleansed_body(&transaction.storage.request,
      &transaction.private_request, transaction.request_copy, response);
  if (transaction.request == NULL) goto cleanup;
  if (strcmp(transaction.request->path, "/zoom/api/v1/auth/login") == 0) {
    if (strcmp(transaction.request->method, "POST") != 0)
      gateway_write_error(response, 405U, "METHOD_NOT_ALLOWED", "POST is required");
    else
      result = gateway_handle_login(gateway, transaction.request, response);
    goto cleanup;
  }
  if (!gateway_state_lock(gateway)) {
    result = 0;
    goto cleanup;
  }
  prepared = prepare_gateway_transaction(gateway, &transaction, response);
  gateway_state_unlock(gateway);
  if (prepared == GATEWAY_PREPARE_FAILED) result = 0;
  else if (prepared == GATEWAY_PREPARE_CONTROL)
    result = execute_gateway_transaction(gateway, &transaction, control, context, response);
cleanup:
  OPENSSL_cleanse(&transaction, sizeof(transaction));
  return result;
}
