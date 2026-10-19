/* Gateway helpers and the unauthenticated request path, exercised through
 * the public gateway.h interface without FastCGI or a running daemon. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"
#include "gateway_test.h"

static int unexpected_control_calls = 0;

static int refuse_control(void *context, uint8_t opcode, uint32_t request_id,
                          const char *payload, char *response,
                          size_t response_capacity) {
  (void)context;
  (void)opcode;
  (void)request_id;
  (void)payload;
  (void)response;
  (void)response_capacity;
  ++unexpected_control_calls;
  return 0;
}

static void test_parse_content_length(void) {
  size_t length = 99U;
  GATEWAY_CHECK(aula_gateway_parse_content_length(NULL, 1024U, &length) && length == 0U);
  GATEWAY_CHECK(aula_gateway_parse_content_length("", 1024U, &length) && length == 0U);
  GATEWAY_CHECK(aula_gateway_parse_content_length("1024", 1024U, &length) && length == 1024U);
  GATEWAY_CHECK(!aula_gateway_parse_content_length("1025", 1024U, &length));
  GATEWAY_CHECK(!aula_gateway_parse_content_length("12x", 1024U, &length));
  GATEWAY_CHECK(!aula_gateway_parse_content_length("x", 1024U, &length));
  GATEWAY_CHECK(!aula_gateway_parse_content_length("1", 1024U, NULL));
}

static void test_redact(void) {
  char output[32];
  aula_gateway_redact(output, sizeof(output), "call failed");
  GATEWAY_CHECK(strcmp(output, "call failed") == 0);
  aula_gateway_redact(output, sizeof(output), "bad PassWord given");
  GATEWAY_CHECK(strcmp(output, "[redacted]") == 0);
  aula_gateway_redact(output, sizeof(output), "uri sip:room@example");
  GATEWAY_CHECK(strcmp(output, "[redacted]") == 0);
  aula_gateway_redact(output, 6U, "truncated text");
  GATEWAY_CHECK(strcmp(output, "trunc") == 0);
  aula_gateway_redact(output, sizeof(output), NULL);
  GATEWAY_CHECK(output[0] == '\0');
}

static void test_oversized_idempotent_reply_is_not_truncated(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  aula_gateway_session session;
  uint8_t request_hash[AULA_GATEWAY_HASH_BYTES] = {1};
  const aula_gateway_idempotency *entry;
  aula_gateway_init(&gateway, NULL);
  (void)memset(&session, 0, sizeof(session));
  session.session_id[0] = 7U;
  response.status = 200U;
  (void)memset(response.body, 'x', 2000U);
  response.body[2000] = '\0';
  gateway_save_idempotency(&gateway, &session, "key-0123456789abcdef", "/zoom/api/v1/calls",
                           request_hash, &response);
  entry = gateway_find_idempotency(&gateway, &session, "key-0123456789abcdef",
                                   "/zoom/api/v1/calls");
  GATEWAY_CHECK(entry != NULL);
  if (entry == NULL) return;
  GATEWAY_CHECK(entry->status == 409U);
  GATEWAY_CHECK(strstr(entry->response, "REPLAY_UNAVAILABLE") != NULL);
  GATEWAY_CHECK(strchr(entry->response, 'x') == NULL);
}

static void test_clock_is_monotonic_and_nonzero(void) {
  uint64_t first = aula_gateway_clock_seconds();
  uint64_t second = aula_gateway_clock_seconds();
  GATEWAY_CHECK(first != 0U);
  GATEWAY_CHECK(second >= first);
}

static void test_unauthenticated_requests_are_refused(void) {
  static aula_gateway gateway;
  aula_gateway_config config;
  aula_gateway_request request;
  static aula_gateway_response response;
  (void)memset(&config, 0, sizeof(config));
  config.allowed_origin = "https://aula.test";
  config.log_line = gateway_test_discard_log;
  aula_gateway_init(&gateway, &config);
  (void)memset(&request, 0, sizeof(request));
  request.method = "GET";
  request.path = "/zoom/api/v1/status";
  request.origin = "https://aula.test";
  request.client_identity = "127.0.0.1";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "";
  request.now = 1000U;
  GATEWAY_CHECK(aula_gateway_handle(&gateway, &request, refuse_control, NULL, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"ok\":false") != NULL);
  GATEWAY_CHECK(response.set_cookie[0] == '\0');
  GATEWAY_CHECK(unexpected_control_calls == 0);
}

int main(void) {
  test_parse_content_length();
  test_redact();
  test_clock_is_monotonic_and_nonzero();
  test_oversized_idempotent_reply_is_not_truncated();
  test_unauthenticated_requests_are_refused();
  return gateway_test_result("test_gateway_basics");
}
