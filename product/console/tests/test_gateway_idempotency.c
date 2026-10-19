/* Idempotency replay and request-schema validation of the console gateway,
 * driven through aula_gateway_handle() with a fake control daemon.  One
 * in-memory operator account is created (PBKDF2 is 600000 iterations). */
#define _POSIX_C_SOURCE 200809L

#include "gateway.h"
#include "gateway_test.h"

#include "aula_sipd/control_protocol.h"

#define TEST_ORIGIN "https://aula.test"
#define OPERATOR_PASSWORD "operator-password-1"
#define KEY_ONE "idempotency-key-0001"
#define KEY_TWO "idempotency-key-0002"
#define KEY_THREE "idempotency-key-0003"

typedef struct fake_control {
  int calls;
  uint8_t last_opcode;
  /* Value returned to the gateway; 0 simulates an unavailable daemon. */
  int result;
} fake_control;

static int fake_control_fn(void *context, uint8_t opcode, uint32_t request_id,
                           const char *payload, char *response,
                           size_t response_capacity) {
  fake_control *fake = context;
  (void)request_id;
  (void)payload;
  ++fake->calls;
  fake->last_opcode = opcode;
  (void)snprintf(response, response_capacity, "%s", "{\"ok\":true}");
  return fake->result;
}

typedef struct session_fixture {
  char cookie[128];
  char csrf[65];
} session_fixture;

static aula_gateway_request base_request(const char *method, const char *path,
                                         uint64_t now) {
  aula_gateway_request request;
  (void)memset(&request, 0, sizeof(request));
  request.method = method;
  request.path = path;
  request.origin = TEST_ORIGIN;
  request.client_identity = "127.0.0.1";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "";
  request.now = now;
  return request;
}

static int login_operator(aula_gateway *gateway, session_fixture *session) {
  static aula_gateway_response response;
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/auth/login", 1000U);
  const char *marker;
  const char *end;
  request.body = "{\"username\":\"operator_user\",\"password\":\"" OPERATOR_PASSWORD "\"}";
  if (!aula_gateway_handle(gateway, &request, NULL, NULL, &response) ||
      response.status != 200U) return 0;
  end = strchr(response.set_cookie, ';');
  marker = strstr(response.body, "\"csrf_token\":\"");
  if (end == NULL || marker == NULL || (size_t)(end - response.set_cookie) >= sizeof(session->cookie))
    return 0;
  (void)memcpy(session->cookie, response.set_cookie, (size_t)(end - response.set_cookie));
  session->cookie[end - response.set_cookie] = '\0';
  marker += strlen("\"csrf_token\":\"");
  (void)memcpy(session->csrf, marker, 64U);
  session->csrf[64] = '\0';
  return 1;
}

/* A DTMF request carrying the session's cookie and CSRF token. */
static aula_gateway_request dtmf_request(const session_fixture *session, const char *body,
                                         const char *key, uint64_t now) {
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/calls/active/dtmf", now);
  request.cookie = session->cookie;
  request.csrf_token = session->csrf;
  request.idempotency_key = key;
  request.body = body;
  return request;
}

static void test_replay_returns_cached_response(aula_gateway *gateway,
                                                const session_fixture *session,
                                                fake_control *fake) {
  static aula_gateway_response first;
  static aula_gateway_response replay;
  aula_gateway_request request = dtmf_request(session, "{\"tone\":\"5\"}", KEY_ONE, 1001U);
  const int calls_before = fake->calls;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &first));
  GATEWAY_CHECK(first.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
  GATEWAY_CHECK(fake->last_opcode == AULA_CONTROL_OPCODE_DTMF);

  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &replay));
  GATEWAY_CHECK(replay.status == 200U);
  GATEWAY_CHECK(strcmp(replay.body, first.body) == 0);
  GATEWAY_CHECK(strcmp(replay.content_type, "application/json") == 0);
  GATEWAY_CHECK(fake->calls == calls_before + 1);

  /* The fingerprint is over the canonical JSON: key order and whitespace do
   * not make a request different. */
  request.body = "{ \"tone\" : \"5\" }";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &replay));
  GATEWAY_CHECK(replay.status == 200U);
  GATEWAY_CHECK(strcmp(replay.body, first.body) == 0);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
}

static void test_conflict_on_different_body(aula_gateway *gateway,
                                            const session_fixture *session,
                                            fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request = dtmf_request(session, "{\"tone\":\"6\"}", KEY_ONE, 1002U);
  const int calls_before = fake->calls;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 409U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"IDEMPOTENCY_CONFLICT\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before);
}

static void test_key_is_scoped_to_route(aula_gateway *gateway, const session_fixture *session,
                                        fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request = base_request("DELETE", "/zoom/api/v1/calls/active", 1003U);
  const int calls_before = fake->calls;
  request.cookie = session->cookie;
  request.csrf_token = session->csrf;
  request.idempotency_key = KEY_ONE;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
  GATEWAY_CHECK(fake->last_opcode == AULA_CONTROL_OPCODE_HANGUP);
  /* ...and it is recorded under that route, so it replays there too. */
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
}

static void test_failed_call_is_not_cached(aula_gateway *gateway, const session_fixture *session,
                                           fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request = dtmf_request(session, "{\"tone\":\"7\"}", KEY_TWO, 1004U);
  const int calls_before = fake->calls;
  fake->result = 0;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 503U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"BACKEND_UNAVAILABLE\"") != NULL);
  fake->result = 1;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 2);
}

static void test_schema_validation(aula_gateway *gateway, const session_fixture *session,
                                   fake_control *fake) {
  static aula_gateway_response response;
  static const char *const invalid_bodies[] = {
      "{\"tone\":\"5\",\"extra\":1}",   /* unknown key */
      "{\"tone\":\"5\",\"tone\":\"6\"}", /* duplicate key */
      "{\"tone\":5}",                   /* wrong type */
      "{\"tone\":\"Z\"}",               /* outside the enum */
      "{\"digit\":\"5\"}",              /* wrong key */
      "{}",                             /* missing key */
      "[\"5\"]",                        /* not an object */
      "not json",
      ""};
  const int calls_before = fake->calls;
  size_t index;
  aula_gateway_request request;
  for (index = 0U; index < sizeof(invalid_bodies) / sizeof(invalid_bodies[0]); ++index) {
    request = dtmf_request(session, invalid_bodies[index], KEY_THREE, 1005U);
    GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
    GATEWAY_CHECK(response.status == 400U);
    GATEWAY_CHECK(strstr(response.body, "\"code\":\"SCHEMA_INVALID\"") != NULL);
  }
  GATEWAY_CHECK(fake->calls == calls_before);

  /* A rejected body is not recorded: the same key is still usable. */
  request = dtmf_request(session, "{\"tone\":\"#\"}", KEY_THREE, 1005U);
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);

  /* Authentication comes before the body schema: an unauthenticated caller
   * is refused with 401 and learns nothing about the schema. */
  request = base_request("POST", "/zoom/api/v1/calls/active/dtmf", 1005U);
  request.body = "{\"tone\":5}";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"SCHEMA_INVALID\"") == NULL);
  /* An authenticated malformed body is still SCHEMA_INVALID, without
   * reaching the daemon. */
  request = dtmf_request(session, "{\"tone\":5}", KEY_THREE, 1005U);
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"SCHEMA_INVALID\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
}

int main(void) {
  static aula_gateway gateway;
  static fake_control fake;
  session_fixture session;
  aula_gateway_config config;
  (void)memset(&config, 0, sizeof(config));
  (void)memset(&session, 0, sizeof(session));
  fake.result = 1;
  config.allowed_origin = TEST_ORIGIN;
  config.log_line = gateway_test_discard_log;
  aula_gateway_init(&gateway, &config);
  GATEWAY_CHECK(aula_gateway_upsert_account(&gateway, "operator_user", OPERATOR_PASSWORD,
                                            AULA_GATEWAY_ROLE_OPERATOR));
  GATEWAY_CHECK(login_operator(&gateway, &session));
  test_replay_returns_cached_response(&gateway, &session, &fake);
  test_conflict_on_different_body(&gateway, &session, &fake);
  test_key_is_scoped_to_route(&gateway, &session, &fake);
  test_failed_call_is_not_cached(&gateway, &session, &fake);
  test_schema_validation(&gateway, &session, &fake);
  return gateway_test_result("test_gateway_idempotency");
}
