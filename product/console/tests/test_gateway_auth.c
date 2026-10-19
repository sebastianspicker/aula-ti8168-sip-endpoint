/* Login, cookie, CSRF, Origin, role and logout behaviour of the console
 * gateway, driven end to end through aula_gateway_handle() with a fake control
 * daemon.  Accounts live in memory only; no account store file is involved.
 * PBKDF2 runs 600000 iterations, so the tests create and log in few accounts. */
#define _POSIX_C_SOURCE 200809L

#include "gateway.h"
#include "gateway_test.h"

#include "aula_sipd/control_protocol.h"

#define TEST_ORIGIN "https://aula.test"
#define VIEWER_PASSWORD "viewer-password-1"
#define OPERATOR_PASSWORD "operator-password-1"
#define IDEMPOTENCY_KEY "idempotency-key-0001"
#define IDEMPOTENCY_KEY_TWO "idempotency-key-0002"

static const char STATUS_JSON[] =
    "{\"call_state\":\"idle\",\"media_state\":\"none\",\"revision\":1,"
    "\"rx_rendering\":false,\"reconnect_attempts\":0,"
    "\"video_transmit_enabled\":false,\"audio_muted\":false,"
    "\"renderer\":{\"healthy\":false,"
    "\"audio\":{\"rendering\":false,\"fresh\":false,\"last_success_ns\":0},"
    "\"video\":{\"rendering\":false,\"fresh\":false,\"last_success_ns\":0}},"
    "\"aec\":{\"delay_state\":\"inactive\",\"available\":false,\"active\":false,"
    "\"processed_frames\":0,\"reference_underflows\":0,\"resets\":0},"
    "\"sip\":{\"profile\":\"private_lab\",\"transport\":\"udp\","
    "\"registration\":\"disabled\",\"status_code\":0,\"expires_seconds\":0},"
    "\"video\":{\"payload_type\":0,\"direction\":\"inactive\",\"security\":\"rtp\","
    "\"h264_profile_level_id\":\"none\",\"frames\":0,\"packets\":0,\"lost\":0,"
    "\"jitter_ns\":0},"
    "\"audio\":{\"payload_type\":0,\"direction\":\"inactive\",\"security\":\"rtp\","
    "\"frames\":0,\"packets\":0,\"lost\":0,\"jitter_ns\":0},\"last_error\":0}";

typedef struct fake_control {
  int calls;
  uint8_t last_opcode;
  char last_payload[128];
} fake_control;

static int fake_control_fn(void *context, uint8_t opcode, uint32_t request_id,
                           const char *payload, char *response,
                           size_t response_capacity) {
  fake_control *fake = context;
  (void)request_id;
  ++fake->calls;
  fake->last_opcode = opcode;
  (void)snprintf(fake->last_payload, sizeof(fake->last_payload), "%s",
                 payload == NULL ? "" : payload);
  (void)snprintf(response, response_capacity, "%s",
                 opcode == AULA_CONTROL_OPCODE_STATUS ? STATUS_JSON : "{\"ok\":true}");
  return 1;
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

static int login(aula_gateway *gateway, const char *username, const char *password,
                 const char *client, uint64_t now, aula_gateway_response *response,
                 session_fixture *session) {
  char body[256];
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/auth/login", now);
  const char *marker;
  const char *end;
  (void)snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}",
                 username, password);
  request.body = body;
  request.client_identity = client;
  if (!aula_gateway_handle(gateway, &request, NULL, NULL, response)) return 0;
  if (session == NULL || response->status != 200U) return 1;
  end = strchr(response->set_cookie, ';');
  if (end == NULL || (size_t)(end - response->set_cookie) >= sizeof(session->cookie)) return 0;
  (void)memcpy(session->cookie, response->set_cookie, (size_t)(end - response->set_cookie));
  session->cookie[end - response->set_cookie] = '\0';
  marker = strstr(response->body, "\"csrf_token\":\"");
  if (marker == NULL) return 0;
  marker += strlen("\"csrf_token\":\"");
  (void)memcpy(session->csrf, marker, 64U);
  session->csrf[64] = '\0';
  return 1;
}

static void test_login_failures(aula_gateway *gateway) {
  static aula_gateway_response response;
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/auth/login", 1000U);
  GATEWAY_CHECK(login(gateway, "viewer_user", "wrong-password-xyz", "10.0.0.1", 1000U,
                      &response, NULL));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"AUTH_INVALID\"") != NULL);
  GATEWAY_CHECK(response.set_cookie[0] == '\0');

  GATEWAY_CHECK(login(gateway, "nobody_here", "whatever-password-1", "10.0.0.2", 1000U,
                      &response, NULL));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"AUTH_INVALID\"") != NULL);

  request.body = "{\"username\":\"viewer_user\",\"password\":\"x\",\"extra\":1}";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, NULL, NULL, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"SCHEMA_INVALID\"") != NULL);

  request.body = "not json";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, NULL, NULL, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"BAD_JSON\"") != NULL);

  request.method = "GET";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, NULL, NULL, &response));
  GATEWAY_CHECK(response.status == 405U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"METHOD_NOT_ALLOWED\"") != NULL);

  request = base_request("POST", "/zoom/api/v1/auth/login", 1000U);
  request.origin = "https://evil.test";
  request.body = "{\"username\":\"viewer_user\",\"password\":\"x\"}";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, NULL, NULL, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"ORIGIN_REQUIRED\"") != NULL);
  GATEWAY_CHECK(response.set_cookie[0] == '\0');
}

static void test_login_success_shape(aula_gateway *gateway, session_fixture *viewer) {
  static aula_gateway_response response;
  static const char prefix[] = "{\"revision\":1,\"ok\":true,\"data\":{\"csrf_token\":\"";
  GATEWAY_CHECK(login(gateway, "viewer_user", VIEWER_PASSWORD, "10.0.0.3", 1000U,
                      &response, viewer));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(strcmp(response.content_type, "application/json") == 0);
  GATEWAY_CHECK(strncmp(response.set_cookie, "aula_session=", 13U) == 0);
  GATEWAY_CHECK(strstr(response.set_cookie, "; Path=/zoom/") != NULL);
  GATEWAY_CHECK(strstr(response.set_cookie, "; Secure") != NULL);
  GATEWAY_CHECK(strstr(response.set_cookie, "; HttpOnly") != NULL);
  GATEWAY_CHECK(strstr(response.set_cookie, "; SameSite=Strict") != NULL);
  GATEWAY_CHECK(strstr(response.set_cookie, "; Max-Age=900") != NULL);
  GATEWAY_CHECK(strncmp(response.body, prefix, strlen(prefix)) == 0);
  GATEWAY_CHECK(strstr(response.body, "\"role\":1}}") != NULL);
  GATEWAY_CHECK(strlen(viewer->cookie) == strlen("aula_session=") + 64U);
  GATEWAY_CHECK(strlen(viewer->csrf) == 64U);
}

static void test_status_requires_session_and_origin(aula_gateway *gateway,
                                                    const session_fixture *viewer,
                                                    fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request = base_request("GET", "/zoom/api/v1/status", 1001U);
  const int calls_before = fake->calls;

  /* No cookie. */
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"UNAUTHORIZED\"") != NULL);

  /* Unknown but well-formed token. */
  request.cookie = "aula_session=0000000000000000000000000000000000000000000000000000000000000000";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 401U);

  /* Foreign Origin. */
  request.cookie = viewer->cookie;
  request.origin = "https://evil.test";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"ORIGIN_REQUIRED\"") != NULL);

  /* Missing Origin without Fetch Metadata. */
  request.origin = NULL;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"ORIGIN_REQUIRED\"") != NULL);

  /* Missing Origin with a non-same-origin Sec-Fetch-Site. */
  request.fetch_site = "cross-site";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(fake->calls == calls_before);

  /* A safe GET may omit Origin only with Sec-Fetch-Site: same-origin. */
  request.fetch_site = "same-origin";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);

  /* The same exemption never applies to a mutation. */
  request = base_request("DELETE", "/zoom/api/v1/calls/active", 1001U);
  request.origin = NULL;
  request.fetch_site = "same-origin";
  request.cookie = viewer->cookie;
  request.csrf_token = viewer->csrf;
  request.idempotency_key = IDEMPOTENCY_KEY;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"ORIGIN_REQUIRED\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
}

static void test_authenticated_status_reaches_control(aula_gateway *gateway,
                                                      const session_fixture *viewer,
                                                      fake_control *fake) {
  static aula_gateway_response response;
  static const char prefix[] = "{\"revision\":1,\"ok\":true,\"data\":{";
  aula_gateway_request request = base_request("GET", "/zoom/api/v1/status", 1002U);
  const int calls_before = fake->calls;
  request.cookie = viewer->cookie;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
  GATEWAY_CHECK(fake->last_opcode == AULA_CONTROL_OPCODE_STATUS);
  GATEWAY_CHECK(strncmp(response.body, prefix, strlen(prefix)) == 0);
  GATEWAY_CHECK(strstr(response.body, "\"call_state\":\"idle\"") != NULL);
  GATEWAY_CHECK(strstr(response.body, "\"registration\":\"disabled\"") != NULL);
  /* The session is within its rotation window: no new cookie is set. */
  GATEWAY_CHECK(response.set_cookie[0] == '\0');

  /* GET /auth/session reports the CSRF token and role without a daemon call. */
  request.path = "/zoom/api/v1/auth/session";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(strstr(response.body, viewer->csrf) != NULL);
  GATEWAY_CHECK(strstr(response.body, "\"role\":1") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
}

static void test_mutation_guards(aula_gateway *gateway, const session_fixture *operator_session,
                                 const session_fixture *viewer, fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request =
      base_request("POST", "/zoom/api/v1/calls/active/dtmf", 1003U);
  const int calls_before = fake->calls;
  request.body = "{\"tone\":\"5\"}";
  request.cookie = operator_session->cookie;
  request.idempotency_key = IDEMPOTENCY_KEY;

  /* Missing CSRF token. */
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"CSRF_INVALID\"") != NULL);

  /* Wrong CSRF token (the viewer's, not the operator's). */
  request.csrf_token = viewer->csrf;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"CSRF_INVALID\"") != NULL);

  /* Valid CSRF, no idempotency key. */
  request.csrf_token = operator_session->csrf;
  request.idempotency_key = NULL;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"IDEMPOTENCY_REQUIRED\"") != NULL);

  /* Too short an idempotency key. */
  request.idempotency_key = "short";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"IDEMPOTENCY_REQUIRED\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before);

  /* All guards satisfied: reaches the daemon with the DTMF opcode and body. */
  request.idempotency_key = IDEMPOTENCY_KEY;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(strstr(response.body, "\"ok\":true,\"data\":{\"ok\":true}") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before + 1);
  GATEWAY_CHECK(fake->last_opcode == AULA_CONTROL_OPCODE_DTMF);
  GATEWAY_CHECK(strcmp(fake->last_payload, "{\"tone\":\"5\"}") == 0);

  /* DELETE /calls/active (hangup) is guarded the same way. */
  request = base_request("DELETE", "/zoom/api/v1/calls/active", 1003U);
  request.cookie = operator_session->cookie;
  request.csrf_token = operator_session->csrf;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 400U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"IDEMPOTENCY_REQUIRED\"") != NULL);
  request.idempotency_key = IDEMPOTENCY_KEY_TWO;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(fake->last_opcode == AULA_CONTROL_OPCODE_HANGUP);
  GATEWAY_CHECK(fake->calls == calls_before + 2);
}

static void test_role_enforcement(aula_gateway *gateway, const session_fixture *viewer,
                                  const session_fixture *operator_session,
                                  fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request =
      base_request("DELETE", "/zoom/api/v1/calls/active", 1004U);
  const int calls_before = fake->calls;

  /* Operator-only mutation as a viewer, with every other guard satisfied. */
  request.cookie = viewer->cookie;
  request.csrf_token = viewer->csrf;
  request.idempotency_key = "idempotency-key-viewer";
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"FORBIDDEN\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before);

  /* Admin-only reads are refused to a viewer and to an operator. */
  request = base_request("GET", "/zoom/api/v1/settings", 1004U);
  request.cookie = viewer->cookie;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"FORBIDDEN\"") != NULL);
  request.cookie = operator_session->cookie;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(fake->calls == calls_before);

  /* Unknown route. */
  request = base_request("GET", "/zoom/api/v1/nope", 1004U);
  request.cookie = viewer->cookie;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 404U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"ROUTE_NOT_FOUND\"") != NULL);
}

static void test_logout_invalidates_cookie(aula_gateway *gateway,
                                           const session_fixture *viewer,
                                           fake_control *fake) {
  static aula_gateway_response response;
  aula_gateway_request request = base_request("POST", "/zoom/api/v1/auth/logout", 1005U);
  const int calls_before = fake->calls;
  request.cookie = viewer->cookie;
  request.idempotency_key = IDEMPOTENCY_KEY;

  /* Logout is a mutation: it needs the CSRF token. */
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 403U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"CSRF_INVALID\"") != NULL);

  request.csrf_token = viewer->csrf;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 200U);
  GATEWAY_CHECK(strstr(response.body, "\"logged_out\":true") != NULL);
  GATEWAY_CHECK(strcmp(response.set_cookie,
                       "aula_session=; Path=/zoom/; Secure; HttpOnly; SameSite=Strict; Max-Age=0") == 0);
  GATEWAY_CHECK(fake->calls == calls_before);

  /* The old cookie no longer authenticates. */
  request = base_request("GET", "/zoom/api/v1/status", 1006U);
  request.cookie = viewer->cookie;
  GATEWAY_CHECK(aula_gateway_handle(gateway, &request, fake_control_fn, fake, &response));
  GATEWAY_CHECK(response.status == 401U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"UNAUTHORIZED\"") != NULL);
  GATEWAY_CHECK(fake->calls == calls_before);
}

int main(void) {
  static aula_gateway gateway;
  static fake_control fake;
  static aula_gateway_response response;
  session_fixture viewer;
  session_fixture operator_session;
  aula_gateway_config config;
  (void)memset(&config, 0, sizeof(config));
  (void)memset(&viewer, 0, sizeof(viewer));
  (void)memset(&operator_session, 0, sizeof(operator_session));
  config.allowed_origin = TEST_ORIGIN;
  config.log_line = gateway_test_discard_log;
  aula_gateway_init(&gateway, &config);
  GATEWAY_CHECK(aula_gateway_upsert_account(&gateway, "viewer_user", VIEWER_PASSWORD,
                                            AULA_GATEWAY_ROLE_VIEWER));
  GATEWAY_CHECK(aula_gateway_upsert_account(&gateway, "operator_user", OPERATOR_PASSWORD,
                                            AULA_GATEWAY_ROLE_OPERATOR));
  test_login_failures(&gateway);
  test_login_success_shape(&gateway, &viewer);
  GATEWAY_CHECK(login(&gateway, "operator_user", OPERATOR_PASSWORD, "10.0.0.4", 1000U,
                      &response, &operator_session));
  GATEWAY_CHECK(response.status == 200U && strstr(response.body, "\"role\":2}}") != NULL);
  test_status_requires_session_and_origin(&gateway, &viewer, &fake);
  test_authenticated_status_reaches_control(&gateway, &viewer, &fake);
  test_mutation_guards(&gateway, &operator_session, &viewer, &fake);
  test_role_enforcement(&gateway, &viewer, &operator_session, &fake);
  test_logout_invalidates_cookie(&gateway, &viewer, &fake);
  return gateway_test_result("test_gateway_auth");
}
