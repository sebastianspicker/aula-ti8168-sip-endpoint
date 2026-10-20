#define _POSIX_C_SOURCE 200809L
#include "gateway_internal.h"
#include "gateway_test.h"
#include "../device/client.h"
#include "../device/transport.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_peer(void) {
  int pair[2];
  char output[256];
  GATEWAY_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  GATEWAY_CHECK(aula_device_peer(pair[0], (uint32_t)geteuid()));
  GATEWAY_CHECK(!aula_device_peer(pair[0], (uint32_t)geteuid() + 1U));
  GATEWAY_CHECK(!aula_device_read_status_fd(pair[0], (uint32_t)geteuid() + 1U,
      output, sizeof(output)));
  close(pair[0]); close(pair[1]);
}

static int last_warning;

static int policy_load(const char *optional, aula_gateway *gateway) {
#if defined(__APPLE__)
  char path[] = "/private/tmp/aula-device-policy-XXXXXX";
#else
  char path[] = "/tmp/aula-device-policy-XXXXXX";
#endif
  int fd = mkstemp(path);
  FILE *file = fd < 0 ? NULL : fdopen(fd, "w");
  aula_gateway_config config = {0};
  int result;
  FILE *logs;
  int saved_stderr;
  char logged[1024] = {0};
  GATEWAY_CHECK(file != NULL);
  if (file == NULL) return 0;
  fprintf(file, "{\"allowed_origin\":\"https://aula.test\","
      "\"control_socket_path\":\"/run/aula-sipd/control.sock\","
      "\"account_store_path\":\"/var/lib/aula-console/account.json\","
      "\"expected_sipd_uid\":1001,\"preview_enabled\":false,"
      "\"preview_rtsp_ipv4\":\"127.0.0.1\",\"preview_rtsp_port\":554%s}", optional);
  fclose(file);
  config.policy_path = path;
  aula_gateway_init(gateway, &config);
  logs = tmpfile();
  GATEWAY_CHECK(logs != NULL);
  saved_stderr = dup(STDERR_FILENO);
  GATEWAY_CHECK(saved_stderr >= 0);
  fflush(stderr);
  if (logs != NULL && saved_stderr >= 0) dup2(fileno(logs), STDERR_FILENO);
  result = aula_gateway_load_policy(gateway);
  fflush(stderr);
  if (saved_stderr >= 0) { dup2(saved_stderr, STDERR_FILENO); close(saved_stderr); }
  if (logs != NULL) {
    rewind(logs);
    (void)fread(logged, 1, sizeof(logged) - 1U, logs);
    fclose(logs);
  }
  last_warning = strstr(logged, "WARNING: gateway DEVELOPMENT device provider enabled") != NULL;
  unlink(path);
  pthread_mutex_destroy(&gateway->control_mutex);
  pthread_mutex_destroy(&gateway->read_control_mutex);
  pthread_mutex_destroy(&gateway->state_mutex);
  return result;
}

static void test_policy(void) {
  static aula_gateway gateway;
  GATEWAY_CHECK(policy_load("", &gateway));
  GATEWAY_CHECK(strcmp(gateway.config.device_socket_path, AULA_DEVICE_SOCKET) == 0);
  GATEWAY_CHECK(gateway.config.expected_device_uid == 0);
  GATEWAY_CHECK(!gateway.config.development_device_provider);
  GATEWAY_CHECK(!last_warning);
  GATEWAY_CHECK(policy_load(",\"device_socket_path\":\"/run/custom-device/control.sock\"", &gateway));
  GATEWAY_CHECK(strcmp(gateway.config.device_socket_path, "/run/custom-device/control.sock") == 0);
  GATEWAY_CHECK(!policy_load(",\"expected_device_uid\":501", &gateway));
  GATEWAY_CHECK(!policy_load(",\"expected_device_uid\":501,\"development_device_provider\":false", &gateway));
  GATEWAY_CHECK(policy_load(",\"expected_device_uid\":501,\"development_device_provider\":true", &gateway));
  GATEWAY_CHECK(gateway.config.expected_device_uid == 501);
  GATEWAY_CHECK(gateway.config.development_device_provider);
  GATEWAY_CHECK(last_warning);
  GATEWAY_CHECK(policy_load(",\"expected_device_uid\":0", &gateway));
  GATEWAY_CHECK(!last_warning);
  GATEWAY_CHECK(!policy_load(",\"expected_device_uid\":-1,\"development_device_provider\":true", &gateway));
  GATEWAY_CHECK(!policy_load(",\"expected_device_uid\":4294967296,\"development_device_provider\":true", &gateway));
  GATEWAY_CHECK(!policy_load(",\"expected_device_uid\":true,\"development_device_provider\":true", &gateway));
  GATEWAY_CHECK(!policy_load(",\"development_device_provider\":1", &gateway));
  GATEWAY_CHECK(!policy_load(",\"device_socket_path\":\"relative.sock\"", &gateway));
  GATEWAY_CHECK(!policy_load(",\"device_socket_path\":\"/run/../bad.sock\"", &gateway));
  GATEWAY_CHECK(!policy_load(",\"unknown_device_key\":true", &gateway));
}

static void test_direct_config(void) {
  static aula_gateway gateway;
  aula_gateway_config config = {0};
  config.expected_device_uid = 501;
  aula_gateway_init(&gateway, &config);
  GATEWAY_CHECK(gateway.config.expected_device_uid == 0);
  GATEWAY_CHECK(strcmp(gateway.config.device_socket_path, AULA_DEVICE_SOCKET) == 0);
  pthread_mutex_destroy(&gateway.control_mutex);
  pthread_mutex_destroy(&gateway.read_control_mutex);
  pthread_mutex_destroy(&gateway.state_mutex);
}

int main(void) {
  test_direct_config();
  test_peer();
  test_policy();
  return gateway_test_result("test_gateway_device");
}
