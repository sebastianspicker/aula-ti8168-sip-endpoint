/* The per-request log: which requests produce a line, what a line may
 * contain, and the per-minute budgets that bound it. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"
#include "gateway_test.h"

typedef struct captured_log {
  unsigned int count;
  char lines[80][256];
} captured_log;

static void capture_line(void *context, const char *line) {
  captured_log *log = (captured_log *)context;
  if (log->count < sizeof(log->lines) / sizeof(log->lines[0]))
    (void)snprintf(log->lines[log->count], sizeof(log->lines[0]), "%s", line);
  ++log->count;
}

static int backend_unavailable(void *context, uint8_t opcode, uint32_t request_id,
                               const char *payload, char *response, size_t capacity) {
  (void)context;
  (void)opcode;
  (void)request_id;
  (void)payload;
  (void)response;
  (void)capacity;
  return 0;
}

static int status_ok(void *context, uint8_t opcode, uint32_t request_id,
                     const char *payload, char *response, size_t capacity) {
  (void)context;
  (void)opcode;
  (void)request_id;
  (void)payload;
  (void)snprintf(response, capacity, "{\"ok\":true}");
  return 1;
}

static void init_gateway(aula_gateway *gateway, captured_log *log) {
  aula_gateway_config config;
  (void)memset(log, 0, sizeof(*log));
  (void)memset(&config, 0, sizeof(config));
  config.allowed_origin = "https://aula.test";
  config.log_line = capture_line;
  config.log_context = log;
  aula_gateway_init(gateway, &config);
}

static aula_gateway_request base_request(const char *method, const char *path, uint64_t now) {
  aula_gateway_request request;
  (void)memset(&request, 0, sizeof(request));
  request.method = method;
  request.path = path;
  request.origin = "https://aula.test";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "";
  request.now = now;
  return request;
}

static void test_failed_login_is_logged_without_credentials(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  static const uint8_t salt[AULA_GATEWAY_SALT_BYTES] = {7};
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/auth/login", 1000U);
  init_gateway(&gateway, &log);
  GATEWAY_CHECK(aula_gateway_set_account(&gateway, "admin", "right password",
                                         AULA_GATEWAY_ROLE_ADMIN, salt));
  request.client_identity = "192.0.2.10";
  request.body = "{\"username\":\"admin\",\"password\":\"hunter2-secret\"}";
  GATEWAY_CHECK(aula_gateway_handle(&gateway, &request, status_ok, NULL, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(log.count == 1U);
  GATEWAY_CHECK(strstr(log.lines[0], "method=POST route=/zoom/api/v1/auth/login "
                                      "status=401 code=AUTH_INVALID client=192.0.2.10 ms=") != NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "hunter2") == NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "admin") == NULL);
}

/* Signs a viewer in directly and stores its cookie header in `cookie`. */
static int sign_in(aula_gateway *gateway, char cookie[96]) {
  static const uint8_t salt[AULA_GATEWAY_SALT_BYTES] = {9};
  char set_cookie[256] = {0};
  char csrf[65] = {0};
  if (!aula_gateway_set_account(gateway, "viewer", "viewer password",
                                AULA_GATEWAY_ROLE_VIEWER, salt) ||
      gateway_new_session(gateway, "viewer", AULA_GATEWAY_ROLE_VIEWER, 1000U,
                          set_cookie, csrf) == NULL)
    return 0;
  (void)snprintf(cookie, 96U, "%.*s", (int)strcspn(set_cookie, ";"), set_cookie);
  return 1;
}

static void test_successful_reads_are_logged_from_their_own_budget(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  char cookie[96];
  unsigned int index;
  /* The session read is answered by the gateway itself, without the daemon. */
  aula_gateway_request request = base_request("GET", "/zoom/api/v1/auth/session", 1020U);
  init_gateway(&gateway, &log);
  GATEWAY_CHECK(sign_in(&gateway, cookie));
  request.cookie = cookie;
  GATEWAY_CHECK(aula_gateway_handle(&gateway, &request, status_ok, NULL, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(log.count == 1U);
  GATEWAY_CHECK(strstr(log.lines[0], "method=GET route=/zoom/api/v1/auth/session status=200 code=-") != NULL);
  /* Polling exhausts the read budget (12 per minute) ... */
  for (index = 0U; index < 70U; ++index)
    (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 12U);
  /* ... but a refused request in the same minute is still logged. */
  request.cookie = NULL;
  (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 13U);
  GATEWAY_CHECK(strstr(log.lines[12], "status=401") != NULL);
  /* The next minute reports what was dropped. */
  request.now = 1080U;
  (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 15U);
  GATEWAY_CHECK(strstr(log.lines[13], "suppressed=59") != NULL);
}

static void test_export_identifiers_are_not_logged(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  aula_gateway_request request = base_request(
      "GET", "/zoom/api/v1/diagnostics/export/0123456789abcdef0123456789abcdef", 1000U);
  init_gateway(&gateway, &log);
  (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 1U);
  GATEWAY_CHECK(strstr(log.lines[0], "route=/zoom/api/v1/diagnostics/export/<id> ") != NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "0123456789abcdef") == NULL);
}

static void test_refused_reads_are_logged(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  aula_gateway_request request = base_request("GET", "/zoom/api/v1/status", 1000U);
  init_gateway(&gateway, &log);
  GATEWAY_CHECK(aula_gateway_handle(&gateway, &request, status_ok, NULL, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(log.count == 1U);
  GATEWAY_CHECK(strstr(log.lines[0], "route=/zoom/api/v1/status status=401 code=UNAUTHORIZED") != NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "client=- ") != NULL);
  /* The query string is never logged. */
  request.path = "/zoom/api/v1/status?token=abc";
  (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 2U);
  GATEWAY_CHECK(strstr(log.lines[1], "route=/zoom/api/v1/status status=") != NULL);
  GATEWAY_CHECK(strstr(log.lines[1], "token") == NULL);
}

static void test_request_text_cannot_forge_lines(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  aula_gateway_request request =
      base_request("GET", "/zoom/x\naula-gateway: forged status=200", 1000U);
  init_gateway(&gateway, &log);
  request.client_identity = "1.2.3.4\nforged";
  (void)aula_gateway_handle(&gateway, &request, status_ok, NULL, &response);
  GATEWAY_CHECK(log.count == 1U);
  GATEWAY_CHECK(strchr(log.lines[0], '\n') == NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "route=/zoom/x?aula-gateway??forged?status") != NULL);
  GATEWAY_CHECK(strstr(log.lines[0], "client=1.2.3.4?") != NULL);
}

static void test_log_volume_is_bounded_per_minute(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static captured_log log;
  unsigned int index;
  aula_gateway_request request = base_request("DELETE", "/zoom/api/v1/calls/active", 6000U);
  init_gateway(&gateway, &log);
  for (index = 0U; index < 35U; ++index)
    (void)aula_gateway_handle(&gateway, &request, backend_unavailable, NULL, &response);
  GATEWAY_CHECK(log.count == 30U);
  request.now = 6060U;
  (void)aula_gateway_handle(&gateway, &request, backend_unavailable, NULL, &response);
  GATEWAY_CHECK(log.count == 32U);
  GATEWAY_CHECK(strstr(log.lines[30], "suppressed=5") != NULL);
  GATEWAY_CHECK(strstr(log.lines[31], "method=DELETE route=/zoom/api/v1/calls/active") != NULL);
}

int main(void) {
  test_failed_login_is_logged_without_credentials();
  test_refused_reads_are_logged();
  test_successful_reads_are_logged_from_their_own_budget();
  test_export_identifiers_are_not_logged();
  test_request_text_cannot_forge_lines();
  test_log_volume_is_bounded_per_minute();
  return gateway_test_result("test_gateway_logging");
}
