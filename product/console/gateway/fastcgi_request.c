#include "fastcgi_request.h"

#include <stddef.h>
#include <string.h>

#define AULA_FASTCGI_METHOD_BYTES 8U
#define AULA_FASTCGI_PATH_BYTES 255U
#define AULA_FASTCGI_ORIGIN_BYTES 127U
#define AULA_FASTCGI_FETCH_SITE_BYTES 32U
#define AULA_FASTCGI_COOKIE_BYTES 1024U
#define AULA_FASTCGI_CSRF_BYTES 64U
#define AULA_FASTCGI_IDEMPOTENCY_BYTES 80U
#define AULA_FASTCGI_CLIENT_BYTES 64U
#define AULA_FASTCGI_SERVER_ADDRESS_BYTES 45U
#define AULA_FASTCGI_SERVER_PORT_BYTES 5U

static int env_value(char *const envp[], const char *name, int required,
                     const char **out_value) {
  size_t index;
  size_t name_length;
  const char *found = NULL;
  if (envp == NULL || name == NULL || out_value == NULL) return 0;
  name_length = strlen(name);
  for (index = 0U; envp[index] != NULL; ++index) {
    if (strncmp(envp[index], name, name_length) == 0 &&
        envp[index][name_length] == '=') {
      if (found != NULL) return 0;
      found = envp[index] + name_length + 1U;
    }
  }
  if (required != 0 && found == NULL) return 0;
  *out_value = found;
  return 1;
}

static size_t bounded_length(const char *value, size_t maximum) {
  size_t length = 0U;
  if (value == NULL) return 0U;
  while (length < maximum && value[length] != '\0') ++length;
  return length;
}

static int bounded_optional(const char *value, size_t maximum) {
  return value == NULL || bounded_length(value, maximum + 1U) <= maximum;
}

static int method_is_supported(const char *method) {
  return method != NULL &&
      (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0 ||
       strcmp(method, "PATCH") == 0 || strcmp(method, "DELETE") == 0);
}

static int path_prefix_and_length_are_valid(const char *path, size_t length) {
  const char prefix[] = "/zoom/api/v1/";
  return length > sizeof(prefix) - 1U && length <= AULA_FASTCGI_PATH_BYTES &&
      strncmp(path, prefix, sizeof(prefix) - 1U) == 0;
}

static int path_has_forbidden_component(const char *path) {
  return strchr(path, '?') != NULL || strchr(path, '#') != NULL ||
      strstr(path, "//") != NULL || strstr(path, "/./") != NULL ||
      strstr(path, "/../") != NULL;
}

static int path_has_dot_tail(const char *path, size_t length) {
  return (length >= 2U && strcmp(path + length - 2U, "/.") == 0) ||
      (length >= 3U && strcmp(path + length - 3U, "/..") == 0);
}

static int canonical_path(const char *document_uri, const char *request_uri,
                          const char *query_string) {
  size_t length;
  if (document_uri == NULL || request_uri == NULL || query_string == NULL ||
      query_string[0] != '\0' || strcmp(document_uri, request_uri) != 0) return 0;
  length = bounded_length(document_uri, AULA_FASTCGI_PATH_BYTES + 1U);
  return path_prefix_and_length_are_valid(document_uri, length) &&
      !path_has_forbidden_component(document_uri) &&
      !path_has_dot_tail(document_uri, length);
}

static int request_values_bounded(const aula_gateway_request *request) {
  return bounded_length(request->method, AULA_FASTCGI_METHOD_BYTES + 1U) <=
             AULA_FASTCGI_METHOD_BYTES &&
      bounded_optional(request->origin, AULA_FASTCGI_ORIGIN_BYTES) &&
      bounded_optional(request->fetch_site, AULA_FASTCGI_FETCH_SITE_BYTES) &&
      bounded_optional(request->cookie, AULA_FASTCGI_COOKIE_BYTES) &&
      bounded_optional(request->csrf_token, AULA_FASTCGI_CSRF_BYTES) &&
      bounded_optional(request->idempotency_key, AULA_FASTCGI_IDEMPOTENCY_BYTES) &&
      bounded_optional(request->client_identity, AULA_FASTCGI_CLIENT_BYTES) &&
      bounded_optional(request->server_address,
                       AULA_FASTCGI_SERVER_ADDRESS_BYTES) &&
      bounded_optional(request->server_port, AULA_FASTCGI_SERVER_PORT_BYTES);
}

static int map_required_environment(char *const envp[],
                                    aula_gateway_request *request,
                                    const char **request_uri,
                                    const char **query_string) {
  return env_value(envp, "REQUEST_METHOD", 1, &request->method) &&
      env_value(envp, "DOCUMENT_URI", 1, &request->path) &&
      env_value(envp, "REQUEST_URI", 1, request_uri) &&
      env_value(envp, "QUERY_STRING", 1, query_string);
}

static int map_optional_environment(char *const envp[],
                                    aula_gateway_request *request) {
  return env_value(envp, "HTTP_ORIGIN", 0, &request->origin) &&
      env_value(envp, "HTTP_SEC_FETCH_SITE", 0, &request->fetch_site) &&
      env_value(envp, "HTTP_COOKIE", 0, &request->cookie) &&
      env_value(envp, "HTTP_X_CSRF_TOKEN", 0, &request->csrf_token) &&
      env_value(envp, "HTTP_IDEMPOTENCY_KEY", 0,
                &request->idempotency_key) &&
      env_value(envp, "AULA_CLIENT_ID", 0, &request->client_identity) &&
      env_value(envp, "SERVER_ADDR", 0, &request->server_address) &&
      env_value(envp, "SERVER_PORT", 0, &request->server_port);
}

int aula_fastcgi_request_map(char *const envp[], const char *body, uint64_t now,
                              aula_gateway_request *out_request) {
  const char *request_uri;
  const char *query_string;
  if (body == NULL || now == 0U || out_request == NULL) return 0;
  (void)memset(out_request, 0, sizeof(*out_request));
  if (!map_required_environment(envp, out_request, &request_uri,
                                &query_string) ||
      !map_optional_environment(envp, out_request)) {
    return 0;
  }
  out_request->body = body;
  out_request->now = now;
  return method_is_supported(out_request->method) &&
      canonical_path(out_request->path, request_uri, query_string) &&
      request_values_bounded(out_request);
}
