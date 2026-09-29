#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <stdio.h>
#include <string.h>

int gateway_valid_request(const aula_gateway *gateway, const aula_gateway_request *request,
                          const aula_gateway_response *response) {
  return gateway != NULL && request != NULL && response != NULL &&
      request->method != NULL && request->path != NULL && request->now != 0U;
}

static int request_body_requires_cleansing(const aula_gateway_request *request) {
  return request != NULL && request->path != NULL &&
      (strcmp(request->path, "/zoom/api/v1/auth/login") == 0 ||
       strcmp(request->path, "/zoom/api/v1/auth/bootstrap") == 0 ||
       strcmp(request->path, "/zoom/api/v1/settings/credentials") == 0 ||
       strcmp(request->path, "/zoom/api/v1/device/credentials") == 0 ||
       strcmp(request->path, "/zoom/api/v1/users") == 0 ||
       strcmp(request->path, "/zoom/api/v1/calls") == 0);
}

const aula_gateway_request *gateway_request_with_cleansed_body(
    const aula_gateway_request *request, aula_gateway_request *private_request,
    char request_copy[1024], aula_gateway_response *response) {
  if (!request_body_requires_cleansing(request)) return request;
  if (request->body == NULL || strlen(request->body) >= 1024U) {
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return NULL;
  }
  (void)snprintf(request_copy, 1024U, "%s", request->body);
  *private_request = *request;
  private_request->body = request_copy;
  return private_request;
}

int gateway_set_redacted_payload(route_plan *plan, const aula_gateway_request *request,
                                 char sensitive[1024], aula_gateway_response *response) {
  if ((plan->flags & R_REDACT) == 0U) return 1;
  if (strlen(request->body) >= 1024U) {
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return 0;
  }
  (void)snprintf(sensitive, 1024U, "%s", request->body);
  plan->payload = sensitive;
  return 1;
}
