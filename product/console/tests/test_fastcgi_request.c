/* The FastCGI environment mapping: which HTTP methods reach the gateway.
 * Compiled without FastCGI; only the pure mapping is linked. */
#define _POSIX_C_SOURCE 200809L

#include "fastcgi_request.h"
#include "gateway_test.h"

static int map_method(char *method_variable) {
  aula_gateway_request request;
  char *envp[] = {
      method_variable,
      "DOCUMENT_URI=/zoom/api/v1/device/credentials",
      "REQUEST_URI=/zoom/api/v1/device/credentials",
      "QUERY_STRING=",
      NULL};
  return aula_fastcgi_request_map(envp, "{}", 1000U, &request);
}

static void test_put_is_accepted_for_the_credentials_route(void) {
  aula_gateway_request request;
  char *envp[] = {
      "REQUEST_METHOD=PUT",
      "DOCUMENT_URI=/zoom/api/v1/device/credentials",
      "REQUEST_URI=/zoom/api/v1/device/credentials",
      "QUERY_STRING=",
      NULL};
  GATEWAY_CHECK(aula_fastcgi_request_map(envp, "{}", 1000U, &request));
  GATEWAY_CHECK(strcmp(request.method, "PUT") == 0);
}

static void test_supported_and_unsupported_methods(void) {
  GATEWAY_CHECK(map_method("REQUEST_METHOD=GET"));
  GATEWAY_CHECK(map_method("REQUEST_METHOD=POST"));
  GATEWAY_CHECK(map_method("REQUEST_METHOD=PATCH"));
  GATEWAY_CHECK(map_method("REQUEST_METHOD=DELETE"));
  GATEWAY_CHECK(!map_method("REQUEST_METHOD=TRACE"));
  GATEWAY_CHECK(!map_method("REQUEST_METHOD=HEAD"));
  GATEWAY_CHECK(!map_method("REQUEST_METHOD=put"));
}

int main(void) {
  test_put_is_accepted_for_the_credentials_route();
  test_supported_and_unsupported_methods();
  return gateway_test_result("test_fastcgi_request");
}
