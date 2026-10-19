/* Login rate-limit bucket tables and the session slot policy of the console
 * gateway. The buckets are exercised through gateway_auth_budget_take() and
 * sessions through gateway_new_session() so no PBKDF2 login is needed, except
 * for one end-to-end refusal check. */
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"
#include "gateway_test.h"

#define TEST_ORIGIN "https://aula.test"

static void init_gateway(aula_gateway *gateway) {
  aula_gateway_config config;
  (void)memset(&config, 0, sizeof(config));
  config.allowed_origin = TEST_ORIGIN;
  config.log_line = gateway_test_discard_log;
  aula_gateway_init(gateway, &config);
}

/* One login attempt's budget; the global bucket is reset so only the
 * 16-entry account and source tables are under test. */
static int take(aula_gateway *gateway, const char *account, const char *source, uint64_t now,
                unsigned int *retry_after) {
  aula_gateway_request request;
  (void)memset(&request, 0, sizeof(request));
  request.client_identity = source;
  request.now = now;
  (void)memset(&gateway->global_bucket, 0, sizeof(gateway->global_bucket));
  return gateway_auth_budget_take(gateway, account, &request, retry_after);
}

static void key_names(unsigned int index, char account[32], char source[32]) {
  (void)snprintf(account, 32U, "account-%u", index);
  (void)snprintf(source, 32U, "10.0.0.%u", index);
}

static void fill_tables(aula_gateway *gateway) {
  char account[32];
  char source[32];
  unsigned int index;
  for (index = 0U; index < AULA_GATEWAY_AUTH_BUCKETS; ++index) {
    key_names(index, account, source);
    GATEWAY_CHECK(take(gateway, account, source, 1000U, NULL));
  }
}

static void test_full_tables_fail_closed_while_penalised(void) {
  static aula_gateway gateway;
  unsigned int retry = 0U;
  init_gateway(&gateway);
  fill_tables(&gateway);
  /* Every bucket carries a penalty (one token taken): a new key is refused
   * instead of overwriting a slot ... */
  GATEWAY_CHECK(!take(&gateway, "account-new", "10.0.1.1", 1001U, &retry));
  GATEWAY_CHECK(retry > 0U && retry <= 30U);
  /* ... so cycling keys does not reset the first key's budget: it had three
   * tokens, one is spent and two remain, then it is exhausted. */
  GATEWAY_CHECK(take(&gateway, "account-0", "10.0.0.0", 1002U, NULL));
  GATEWAY_CHECK(take(&gateway, "account-0", "10.0.0.0", 1003U, NULL));
  GATEWAY_CHECK(!take(&gateway, "account-0", "10.0.0.0", 1004U, &retry));
  GATEWAY_CHECK(retry > 0U);
}

static void test_full_tables_evict_only_refilled_buckets(void) {
  static aula_gateway gateway;
  unsigned int retry = 0U;
  init_gateway(&gateway);
  fill_tables(&gateway);
  /* Drain bucket 0 so it is still penalised when the others have refilled. */
  GATEWAY_CHECK(take(&gateway, "account-0", "10.0.0.0", 1000U, NULL));
  GATEWAY_CHECK(take(&gateway, "account-0", "10.0.0.0", 1000U, NULL));
  GATEWAY_CHECK(!take(&gateway, "account-0", "10.0.0.0", 1000U, NULL));
  /* One refill period later every other bucket is back at capacity, so a new
   * key evicts one of those, never the penalised bucket 0. */
  GATEWAY_CHECK(take(&gateway, "account-new", "10.0.1.1", 1030U, &retry));
  GATEWAY_CHECK(take(&gateway, "account-0", "10.0.0.0", 1030U, NULL));
  GATEWAY_CHECK(!take(&gateway, "account-0", "10.0.0.0", 1030U, &retry));
  /* The newcomer has its own full budget: three tokens, one already spent. */
  GATEWAY_CHECK(take(&gateway, "account-new", "10.0.1.1", 1030U, NULL));
  GATEWAY_CHECK(take(&gateway, "account-new", "10.0.1.1", 1030U, NULL));
  GATEWAY_CHECK(!take(&gateway, "account-new", "10.0.1.1", 1030U, NULL));
}

static int has_session(const aula_gateway *gateway, const char *username) {
  size_t index;
  for (index = 0U; index < AULA_GATEWAY_MAX_SESSIONS; ++index)
    if (gateway->sessions[index].used && strcmp(gateway->sessions[index].username, username) == 0)
      return 1;
  return 0;
}

static aula_gateway_session *open_session(aula_gateway *gateway, const char *username,
                                          aula_gateway_role role, uint64_t now) {
  char cookie[256];
  char csrf[65];
  return gateway_new_session(gateway, username, role, now, cookie, csrf);
}

static void test_viewer_cannot_evict_admin(void) {
  static aula_gateway gateway;
  init_gateway(&gateway);
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1000U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_two", AULA_GATEWAY_ROLE_ADMIN, 1001U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_three", AULA_GATEWAY_ROLE_ADMIN, 1002U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_four", AULA_GATEWAY_ROLE_ADMIN, 1003U) != NULL);
  GATEWAY_CHECK(!gateway_session_slot_available(&gateway, "newcomer", AULA_GATEWAY_ROLE_VIEWER, 1004U));
  GATEWAY_CHECK(!gateway_session_slot_available(&gateway, "newcomer", AULA_GATEWAY_ROLE_OPERATOR, 1004U));
  GATEWAY_CHECK(open_session(&gateway, "viewer_one", AULA_GATEWAY_ROLE_VIEWER, 1004U) == NULL);
  GATEWAY_CHECK(open_session(&gateway, "operator_one", AULA_GATEWAY_ROLE_OPERATOR, 1004U) == NULL);
  GATEWAY_CHECK(has_session(&gateway, "admin_one") && has_session(&gateway, "admin_four"));
  /* Admins evict the least-recently-seen admin. */
  GATEWAY_CHECK(open_session(&gateway, "admin_five", AULA_GATEWAY_ROLE_ADMIN, 1005U) != NULL);
  GATEWAY_CHECK(!has_session(&gateway, "admin_one") && has_session(&gateway, "admin_five"));
}

static void test_expired_sessions_free_their_slots(void) {
  static aula_gateway gateway;
  const uint64_t later = 1003U + AULA_GATEWAY_SESSION_IDLE_SECONDS + 1U;
  init_gateway(&gateway);
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1000U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_two", AULA_GATEWAY_ROLE_ADMIN, 1001U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_three", AULA_GATEWAY_ROLE_ADMIN, 1002U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_four", AULA_GATEWAY_ROLE_ADMIN, 1003U) != NULL);
  /* Once the admins have been idle past the timeout their slots are free. */
  GATEWAY_CHECK(gateway_session_slot_available(&gateway, "newcomer", AULA_GATEWAY_ROLE_VIEWER, later));
  GATEWAY_CHECK(open_session(&gateway, "viewer_one", AULA_GATEWAY_ROLE_VIEWER, later) != NULL);
}

static void test_one_user_cannot_hold_every_slot(void) {
  static aula_gateway gateway;
  init_gateway(&gateway);
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1000U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1001U) != NULL);
  /* A third login by the same admin replaces its own oldest session, so two
   * slots stay available to everyone else. */
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1002U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1003U) != NULL);
  GATEWAY_CHECK(gateway_session_slot_available(&gateway, "viewer_one", AULA_GATEWAY_ROLE_VIEWER, 1004U));
  GATEWAY_CHECK(open_session(&gateway, "viewer_one", AULA_GATEWAY_ROLE_VIEWER, 1004U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "viewer_two", AULA_GATEWAY_ROLE_VIEWER, 1005U) != NULL);
  GATEWAY_CHECK(has_session(&gateway, "admin_one") && has_session(&gateway, "viewer_one"));
}

static void test_admin_evicts_lowest_oldest_viewer(void) {
  static aula_gateway gateway;
  init_gateway(&gateway);
  GATEWAY_CHECK(open_session(&gateway, "viewer_old", AULA_GATEWAY_ROLE_VIEWER, 1000U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_old", AULA_GATEWAY_ROLE_ADMIN, 1001U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "viewer_new", AULA_GATEWAY_ROLE_VIEWER, 1002U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "operator_one", AULA_GATEWAY_ROLE_OPERATOR, 1003U) != NULL);
  GATEWAY_CHECK(gateway_session_slot_available(&gateway, "newcomer", AULA_GATEWAY_ROLE_ADMIN, 1004U));
  /* The oldest session overall is a viewer; an admin takes its slot. */
  GATEWAY_CHECK(open_session(&gateway, "admin_new", AULA_GATEWAY_ROLE_ADMIN, 1004U) != NULL);
  GATEWAY_CHECK(!has_session(&gateway, "viewer_old"));
  GATEWAY_CHECK(has_session(&gateway, "admin_old") && has_session(&gateway, "viewer_new"));
  /* A viewer cannot take the admin's or operator's slot but may replace the
   * remaining viewer. */
  GATEWAY_CHECK(open_session(&gateway, "viewer_third", AULA_GATEWAY_ROLE_VIEWER, 1005U) != NULL);
  GATEWAY_CHECK(!has_session(&gateway, "viewer_new"));
  GATEWAY_CHECK(has_session(&gateway, "admin_old") && has_session(&gateway, "operator_one") &&
                has_session(&gateway, "admin_new"));
}

static void test_same_role_eviction_still_works(void) {
  static aula_gateway gateway;
  unsigned int index;
  char name[32];
  init_gateway(&gateway);
  for (index = 0U; index < AULA_GATEWAY_MAX_SESSIONS; ++index) {
    (void)snprintf(name, sizeof(name), "viewer_%u", index);
    GATEWAY_CHECK(open_session(&gateway, name, AULA_GATEWAY_ROLE_VIEWER, 1000U + index) != NULL);
  }
  GATEWAY_CHECK(open_session(&gateway, "viewer_late", AULA_GATEWAY_ROLE_VIEWER, 1010U) != NULL);
  GATEWAY_CHECK(!has_session(&gateway, "viewer_0") && has_session(&gateway, "viewer_1") &&
                has_session(&gateway, "viewer_late"));
}

static void test_login_is_refused_with_session_limit(void) {
  static aula_gateway gateway;
  static aula_gateway_response response;
  static const uint8_t salt[AULA_GATEWAY_SALT_BYTES] = {5};
  aula_gateway_request request;
  init_gateway(&gateway);
  GATEWAY_CHECK(aula_gateway_set_account(&gateway, "viewer_user", "viewer password",
                                         AULA_GATEWAY_ROLE_VIEWER, salt));
  GATEWAY_CHECK(open_session(&gateway, "admin_one", AULA_GATEWAY_ROLE_ADMIN, 1000U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_two", AULA_GATEWAY_ROLE_ADMIN, 1001U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_three", AULA_GATEWAY_ROLE_ADMIN, 1002U) != NULL);
  GATEWAY_CHECK(open_session(&gateway, "admin_four", AULA_GATEWAY_ROLE_ADMIN, 1003U) != NULL);
  (void)memset(&request, 0, sizeof(request));
  request.method = "POST";
  request.path = "/zoom/api/v1/auth/login";
  request.origin = TEST_ORIGIN;
  request.client_identity = "192.0.2.10";
  request.server_address = "127.0.0.1";
  request.server_port = "443";
  request.body = "{\"username\":\"viewer_user\",\"password\":\"viewer password\"}";
  request.now = 1004U;
  GATEWAY_CHECK(aula_gateway_handle(&gateway, &request, NULL, NULL, &response));
  GATEWAY_CHECK(response.status == 503U);
  GATEWAY_CHECK(strstr(response.body, "\"code\":\"SESSION_LIMIT\"") != NULL);
  GATEWAY_CHECK(response.set_cookie[0] == '\0');
  GATEWAY_CHECK(has_session(&gateway, "admin_one"));
}

int main(void) {
  test_full_tables_fail_closed_while_penalised();
  test_full_tables_evict_only_refilled_buckets();
  test_viewer_cannot_evict_admin();
  test_expired_sessions_free_their_slots();
  test_one_user_cannot_hold_every_slot();
  test_admin_evicts_lowest_oldest_viewer();
  test_same_role_eviction_still_works();
  test_login_is_refused_with_session_limit();
  return gateway_test_result("test_gateway_auth_limits");
}
