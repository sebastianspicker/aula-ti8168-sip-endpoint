/* Bounded, redacted per-request log: one line per request handled by
 * aula_gateway_handle. Lines carry only the method, the route path, the
 * status, the error code, the nginx-supplied client address and the
 * duration: never bodies, cookies, CSRF tokens, idempotency keys, usernames,
 * query strings or diagnostics export identifiers. Two fixed budgets per
 * minute bound the volume: one for state-changing and refused or failed
 * requests (the audit trail) and a separate one for successful reads, so
 * the console's continuous polling cannot crowd out the audit lines. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define GATEWAY_LOG_WINDOW_SECONDS 60U
#define GATEWAY_LOG_AUDIT_LINES_PER_WINDOW 30U
#define GATEWAY_LOG_READ_LINES_PER_WINDOW 12U
#define GATEWAY_LOG_EXPORT_PREFIX "/zoom/api/v1/diagnostics/export/"

/* Copies `source` up to `stop` (or its end), replacing any character outside
 * `allowed` so request-controlled text cannot forge or split a log line. */
static void copy_field(char *destination, size_t capacity, const char *source,
                       const char *allowed, char stop) {
  size_t index = 0U;
  if (source == NULL || source[0] == '\0') {
    (void)snprintf(destination, capacity, "-");
    return;
  }
  for (; source[index] != '\0' && source[index] != stop && index + 1U < capacity; ++index)
    destination[index] = strchr(allowed, source[index]) != NULL ? source[index] : '?';
  destination[index] = '\0';
}

static void response_code(char *destination, size_t capacity, const char *body) {
  const char *code = strstr(body, "\"code\":\"");
  size_t index = 0U;
  if (code == NULL) {
    (void)snprintf(destination, capacity, "-");
    return;
  }
  code += 8;
  for (; ((code[index] >= 'A' && code[index] <= 'Z') || code[index] == '_') &&
         index + 1U < capacity; ++index)
    destination[index] = code[index];
  destination[index] = '\0';
  if (index == 0U) (void)snprintf(destination, capacity, "-");
}

static int request_is_audit(const aula_gateway_request *request,
                            const aula_gateway_response *response, int handled) {
  return !handled || response->status >= 400U || request->method == NULL ||
      strcmp(request->method, "GET") != 0;
}

/* Takes one line from the audit or read budget for the current minute;
 * reports lines either budget dropped in the previous minute through
 * `suppressed`. */
static int take_log_budget(aula_gateway *gateway, uint64_t now, int audit,
                           unsigned int *suppressed) {
  uint64_t window = now / GATEWAY_LOG_WINDOW_SECONDS;
  unsigned int *lines = audit ? &gateway->log_lines : &gateway->log_read_lines;
  unsigned int limit = audit ? GATEWAY_LOG_AUDIT_LINES_PER_WINDOW
                             : GATEWAY_LOG_READ_LINES_PER_WINDOW;
  int allowed;
  *suppressed = 0U;
  if (!gateway_state_lock(gateway)) return 0;
  if (window != gateway->log_window) {
    *suppressed = gateway->log_suppressed;
    gateway->log_window = window;
    gateway->log_lines = 0U;
    gateway->log_read_lines = 0U;
    gateway->log_suppressed = 0U;
  }
  allowed = *lines < limit;
  if (allowed) ++*lines;
  else if (gateway->log_suppressed < UINT_MAX) ++gateway->log_suppressed;
  gateway_state_unlock(gateway);
  return allowed;
}

static void emit(const aula_gateway *gateway, const char *line) {
  if (gateway->config.log_line != NULL)
    gateway->config.log_line(gateway->config.log_context, line);
  else
    (void)fprintf(stderr, "%s\n", line);
}

void gateway_log_request(aula_gateway *gateway, const aula_gateway_request *request,
                         const aula_gateway_response *response, int handled,
                         uint64_t elapsed_milliseconds) {
  static const char route_characters[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_.-";
  char method[8];
  char route[65];
  char client[46];
  char code[33];
  char line[256];
  unsigned int suppressed;
  if (gateway == NULL || request == NULL || response == NULL ||
      !take_log_budget(gateway, request->now,
                       request_is_audit(request, response, handled), &suppressed))
    return;
  if (suppressed != 0U) {
    (void)snprintf(line, sizeof(line),
                   "aula-gateway: request log suppressed=%u in the previous minute", suppressed);
    emit(gateway, line);
  }
  copy_field(method, sizeof(method), request->method, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", '\0');
  if (request->path != NULL &&
      strncmp(request->path, GATEWAY_LOG_EXPORT_PREFIX, strlen(GATEWAY_LOG_EXPORT_PREFIX)) == 0)
    (void)snprintf(route, sizeof(route), "%s<id>", GATEWAY_LOG_EXPORT_PREFIX);
  else
    copy_field(route, sizeof(route), request->path, route_characters, '?');
  copy_field(client, sizeof(client), request->client_identity, "0123456789abcdefABCDEF.:", '\0');
  if (handled) response_code(code, sizeof(code), response->body);
  else (void)snprintf(code, sizeof(code), "GATEWAY_FAILURE");
  (void)snprintf(line, sizeof(line),
                 "aula-gateway: request method=%s route=%s status=%u code=%s client=%s ms=%llu",
                 method, route, handled ? response->status : 500U, code, client,
                 (unsigned long long)elapsed_milliseconds);
  emit(gateway, line);
}
