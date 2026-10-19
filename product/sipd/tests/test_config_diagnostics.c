/* Configuration rejection diagnostics: each failure names its line and
 * setting so an operator can fix it, and never echoes a configured value. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "aula_sipd/config.h"
#include "aula_test.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static char test_directory[96];
static char test_file[128];

static int write_config(const char *text) {
  int descriptor;
  size_t length = strlen(text);
  (void)unlink(test_file);
  descriptor = open(test_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) return 0;
  if (write(descriptor, text, length) != (ssize_t)length) {
    (void)close(descriptor);
    return 0;
  }
  return close(descriptor) == 0;
}

static aula_status load(const char *text, aula_config_diagnostic *diagnostic) {
  aula_config *config = NULL;
  aula_status status;
  if (!write_config(text)) return AULA_STATUS_IO_ERROR;
  status = aula_config_load_file_diagnosed(test_file, &config, diagnostic);
  if (config != NULL) aula_config_destroy(config);
  return status;
}

static void test_parse_errors_name_line_and_setting(void) {
  aula_config_diagnostic diagnostic;
  AULA_CHECK_STATUS(load("# comment\n[runtime]\nbogus = 1\n", &diagnostic),
                    AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK(diagnostic.status == AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK(diagnostic.line == 3U);
  AULA_CHECK(strcmp(diagnostic.setting, "runtime.bogus") == 0);
  AULA_CHECK(diagnostic.reason != NULL);
  AULA_CHECK_STATUS(load("foreground = true\n", &diagnostic), AULA_STATUS_INVALID_DATA);
  AULA_CHECK(diagnostic.line == 1U);
  AULA_CHECK(strcmp(diagnostic.setting, "foreground") == 0);
  AULA_CHECK_STATUS(load("\n[nope]\n", &diagnostic), AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK(diagnostic.line == 2U);
  AULA_CHECK(strcmp(diagnostic.setting, "[nope]") == 0);
}

static void test_values_are_never_echoed(void) {
  aula_config_diagnostic diagnostic;
  AULA_CHECK_STATUS(load("[sip]\nauth_username = hunter2 = again\n", &diagnostic),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK(diagnostic.line == 2U);
  AULA_CHECK(strcmp(diagnostic.setting, "sip.auth_username") == 0);
  AULA_CHECK(strstr(diagnostic.setting, "hunter2") == NULL);
  AULA_CHECK(diagnostic.reason == NULL || strstr(diagnostic.reason, "hunter2") == NULL);
  /* A line without '=' may be a mistyped value: only its section is named. */
  AULA_CHECK_STATUS(load("[sip]\nuri sip:alice@example.test\n", &diagnostic),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK(diagnostic.line == 2U);
  AULA_CHECK(strcmp(diagnostic.setting, "[sip]") == 0);
  AULA_CHECK_STATUS(load("[sip]\nauth_username alice = x\n", &diagnostic),
                    AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK(strcmp(diagnostic.setting, "sip.auth_username") == 0);
  AULA_CHECK_STATUS(load("hunter2-secret\n", &diagnostic), AULA_STATUS_INVALID_DATA);
  AULA_CHECK(strcmp(diagnostic.setting, "line") == 0);
  AULA_CHECK_STATUS(load("[sip hunter2\n", &diagnostic), AULA_STATUS_INVALID_DATA);
  AULA_CHECK(strcmp(diagnostic.setting, "section header") == 0);
}

static void test_validation_errors_name_the_setting(void) {
  aula_config_diagnostic diagnostic;
  /* Parses cleanly; the pid file's parent directory does not exist. */
  char text[256];
  (void)snprintf(text, sizeof(text),
                 "[runtime]\nforeground = true\npid_file = %s/missing/aula.pid\n",
                 test_directory);
  AULA_CHECK(load(text, &diagnostic) != AULA_STATUS_OK);
  AULA_CHECK(diagnostic.line == 0U);
  AULA_CHECK(strcmp(diagnostic.setting, "runtime.pid_file") == 0);
  AULA_CHECK(diagnostic.reason != NULL);
}

static void test_control_requires_a_dedicated_gateway_uid(void) {
  aula_config_diagnostic diagnostic;
  char text[512];
  static const char *const rejected[] = {"0", "65534"};
  size_t index;
  for (index = 0U; index < 2U; ++index) {
    (void)snprintf(text, sizeof(text),
                   "[runtime]\nforeground = true\npid_file = %s/aula.pid\n"
                   "[control]\nenable_local_control = true\n"
                   "unix_socket_path = %s/control.sock\ngateway_uid = %s\n",
                   test_directory, test_directory, rejected[index]);
    AULA_CHECK_STATUS(load(text, &diagnostic), AULA_STATUS_CONFIGURATION_ERROR);
    AULA_CHECK(strcmp(diagnostic.setting, "control.gateway_uid") == 0);
  }
  (void)snprintf(text, sizeof(text),
                 "[runtime]\nforeground = true\npid_file = %s/aula.pid\n"
                 "[control]\nenable_local_control = true\n"
                 "unix_socket_path = %s/control.sock\ngateway_uid = 1201\n",
                 test_directory, test_directory);
  AULA_CHECK_STATUS(load(text, &diagnostic), AULA_STATUS_OK);
  /* With local control disabled the inert default stays acceptable. */
  (void)snprintf(text, sizeof(text),
                 "[runtime]\nforeground = true\npid_file = %s/aula.pid\n"
                 "[control]\ngateway_uid = 65534\n", test_directory);
  AULA_CHECK_STATUS(load(text, &diagnostic), AULA_STATUS_OK);
}

static void test_file_errors_are_reported(void) {
  aula_config *config = NULL;
  aula_config_diagnostic diagnostic;
  AULA_CHECK(aula_config_load_file_diagnosed("relative.conf", &config, &diagnostic) !=
             AULA_STATUS_OK);
  AULA_CHECK(strcmp(diagnostic.setting, "file") == 0);
  AULA_CHECK(diagnostic.reason != NULL && strstr(diagnostic.reason, "absolute") != NULL);
  AULA_CHECK(write_config("[runtime]\n"));
  AULA_CHECK(chmod(test_file, 0622) == 0);
  AULA_CHECK(aula_config_load_file_diagnosed(test_file, &config, &diagnostic) ==
             AULA_STATUS_PERMISSION_DENIED);
  AULA_CHECK(strcmp(diagnostic.setting, "file") == 0);
  AULA_CHECK(config == NULL);
}

int main(void) {
  const char *base = getenv("TMPDIR");
  if (base == NULL || base[0] != '/' || strlen(base) > 40U) base = "/tmp";
  (void)snprintf(test_directory, sizeof(test_directory),
                 "%s/aula-config-diag-XXXXXX", base);
  if (mkdtemp(test_directory) == NULL) {
    (void)fprintf(stderr, "test_config_diagnostics: mkdtemp failed\n");
    return EXIT_FAILURE;
  }
  (void)snprintf(test_file, sizeof(test_file), "%s/aula-sipd.conf", test_directory);
  test_parse_errors_name_line_and_setting();
  test_values_are_never_echoed();
  test_validation_errors_name_the_setting();
  test_control_requires_a_dedicated_gateway_uid();
  test_file_errors_are_reported();
  /* Only the one file this test created and its mkdtemp directory. */
  (void)unlink(test_file);
  (void)rmdir(test_directory);
  return aula_test_result("test_config_diagnostics");
}
