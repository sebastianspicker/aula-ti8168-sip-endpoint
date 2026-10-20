/* Only the pathname connection is replaced: no listener is needed to verify
 * that the gateway supplies operator policy to both device client operations. */
#include "gateway_internal.h"
#include "gateway_test.h"
#include "../device/transport.h"

static char connected_path[108];
static uint32_t connected_uid;
static unsigned calls;

int aula_device_connect(const char *path, uint32_t owner, unsigned timeout_ms) {
  (void)timeout_ms;
  snprintf(connected_path, sizeof(connected_path), "%s", path);
  connected_uid = owner;
  ++calls;
  return -1;
}

int main(void) {
  aula_gateway_config config = {0};
  gateway_transaction transaction;
  aula_gateway_request request = {0};
  char backend[1024];
  memset(&transaction, 0, sizeof(transaction));
  transaction.request = &request;
  snprintf(transaction.principal.username, sizeof(transaction.principal.username), "admin");
  config.device_socket_path = "/run/development-device/control.sock";
  config.expected_device_uid = 501;
  config.development_device_provider = 1;
  request.path = "/zoom/api/v1/device/status";
  GATEWAY_CHECK(!gateway_device_backend_reply(&config, &transaction, backend, sizeof(backend)));
  GATEWAY_CHECK(calls == 1 && connected_uid == 501);
  GATEWAY_CHECK(strcmp(connected_path, config.device_socket_path) == 0);
  request.path = "/zoom/api/v1/device/credentials";
  GATEWAY_CHECK(!gateway_device_backend_reply(&config, &transaction, backend, sizeof(backend)));
  GATEWAY_CHECK(calls == 2 && connected_uid == 501);
  GATEWAY_CHECK(strcmp(connected_path, config.device_socket_path) == 0);
  transaction.plan.flags = R_MUTATION;
  request.body = "{}";
  GATEWAY_CHECK(!gateway_device_backend_reply(&config, &transaction, backend, sizeof(backend)));
  GATEWAY_CHECK(calls == 3 && connected_uid == 501);
  config.development_device_provider = 0;
  GATEWAY_CHECK(!gateway_device_backend_reply(&config, &transaction, backend, sizeof(backend)));
  GATEWAY_CHECK(calls == 4 && connected_uid == 0);
  return gateway_test_result("test_gateway_device_routing");
}
