/* Sliding session renewal as a browser sees it: the session cookie expires
 * Max-Age seconds after the browser receives it, so an active console must
 * be handed a renewed cookie before the current one runs out. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"
#include "gateway_test.h"

typedef struct browser_cookie {
  char header[96];
  uint64_t expires_at;
} browser_cookie;

/* Stores a Set-Cookie value the way a browser would; 0 if it is malformed. */
static int browser_store(browser_cookie *cookie, const char *set_cookie, uint64_t now) {
  const char *max_age = strstr(set_cookie, "; Max-Age=");
  const char *end = strchr(set_cookie, ';');
  unsigned long seconds;
  if (strncmp(set_cookie, "aula_session=", 13U) != 0 || max_age == NULL || end == NULL ||
      (size_t)(end - set_cookie) >= sizeof(cookie->header))
    return 0;
  seconds = strtoul(max_age + 10, NULL, 10);
  (void)snprintf(cookie->header, sizeof(cookie->header), "%.*s",
                 (int)(end - set_cookie), set_cookie);
  cookie->expires_at = now + seconds;
  return seconds > 0U;
}

static void test_polling_console_stays_signed_in(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static const uint8_t salt[AULA_GATEWAY_SALT_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8,
                                                        9, 10, 11, 12, 13, 14, 15, 16};
  aula_gateway_config config;
  aula_gateway_request request;
  browser_cookie cookie;
  char set_cookie[256] = {0};
  char csrf[65] = {0};
  const uint64_t login_at = 10000U;
  uint64_t now;
  int signed_in = 1;
  (void)memset(&config, 0, sizeof(config));
  config.allowed_origin = "https://aula.test";
  aula_gateway_init(&gateway, &config);
  GATEWAY_CHECK(aula_gateway_set_account(&gateway, "viewer", "correct horse battery",
                                         AULA_GATEWAY_ROLE_VIEWER, salt));
  GATEWAY_CHECK(gateway_new_session(&gateway, "viewer", AULA_GATEWAY_ROLE_VIEWER,
                                    login_at, set_cookie, csrf) != NULL);
  GATEWAY_CHECK(browser_store(&cookie, set_cookie, login_at));
  (void)memset(&request, 0, sizeof(request));
  request.method = "GET";
  request.path = "/zoom/api/v1/status";
  request.origin = "https://aula.test";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "";
  /* The console polls every five seconds for twenty minutes. */
  for (now = login_at + 5U; now <= login_at + 20U * 60U; now += 5U) {
    if (now >= cookie.expires_at) {
      (void)fprintf(stderr, "cookie expired in the browser %llu s after login\n",
                    (unsigned long long)(now - login_at));
      signed_in = 0;
      break;
    }
    request.cookie = cookie.header;
    request.now = now;
    (void)memset(&response, 0, sizeof(response));
    if (gateway_authenticate(&gateway, &request, &response, 0) == NULL) {
      (void)fprintf(stderr, "request refused %llu s after login: %u %s\n",
                    (unsigned long long)(now - login_at), response.status, response.body);
      signed_in = 0;
      break;
    }
    if (response.set_cookie[0] != '\0')
      GATEWAY_CHECK(browser_store(&cookie, response.set_cookie, now));
  }
  GATEWAY_CHECK(signed_in);
}

static void test_idle_console_is_signed_out(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static const uint8_t salt[AULA_GATEWAY_SALT_BYTES] = {16, 15, 14, 13, 12, 11, 10, 9,
                                                        8, 7, 6, 5, 4, 3, 2, 1};
  aula_gateway_config config;
  aula_gateway_request request;
  browser_cookie cookie;
  char set_cookie[256] = {0};
  char csrf[65] = {0};
  const uint64_t login_at = 10000U;
  (void)memset(&config, 0, sizeof(config));
  config.allowed_origin = "https://aula.test";
  aula_gateway_init(&gateway, &config);
  GATEWAY_CHECK(aula_gateway_set_account(&gateway, "viewer", "correct horse battery",
                                         AULA_GATEWAY_ROLE_VIEWER, salt));
  GATEWAY_CHECK(gateway_new_session(&gateway, "viewer", AULA_GATEWAY_ROLE_VIEWER,
                                    login_at, set_cookie, csrf) != NULL);
  GATEWAY_CHECK(browser_store(&cookie, set_cookie, login_at));
  (void)memset(&request, 0, sizeof(request));
  request.method = "GET";
  request.path = "/zoom/api/v1/status";
  request.origin = "https://aula.test";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "";
  request.cookie = cookie.header;
  /* The server-side idle timeout still applies even if a client keeps the
   * cookie past its Max-Age. */
  request.now = login_at + AULA_GATEWAY_SESSION_IDLE_SECONDS + 1U;
  GATEWAY_CHECK(gateway_authenticate(&gateway, &request, &response, 0) == NULL);
  GATEWAY_CHECK(response.status == 401U);
}

int main(void) {
  test_polling_console_stays_signed_in();
  test_idle_console_is_signed_out();
  return gateway_test_result("test_gateway_session_renewal");
}
