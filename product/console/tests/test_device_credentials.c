#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "gateway_test.h"
#include "../device/credentials_store.h"
#include <fcntl.h>
#include <jansson.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_NAME "oem-credentials.json"

static void make_key(unsigned n, char key[65]) {
  snprintf(key, 65, "%064x", n);
}

static const char *update(unsigned n, int revision) {
  char key[65];
  json_t *arguments = json_pack("{s:s,s:s,s:i}", "username", "admin", "password", "secret",
      "revision", revision);
  const char *outcome;
  make_key(n, key);
  outcome = aula_device_credentials_update(arguments, key);
  json_decref(arguments);
  return outcome;
}

static int status_revision(void) {
  json_t *status = aula_device_credentials_status("unknown");
  int revision = status == NULL ? -1 : (int)json_integer_value(json_object_get(status, "revision"));
  json_decref(status);
  return revision;
}

static void test_eviction(int directory) {
  char key[65];
  json_t *receipt;
  GATEWAY_CHECK(aula_device_credentials_open(directory));
  for (unsigned n = 1; n <= 40; ++n)
    GATEWAY_CHECK(strcmp(update(n, (int)n), "succeeded") == 0);
  GATEWAY_CHECK(status_revision() == 41);
  GATEWAY_CHECK(strcmp(update(40, 40), "duplicate") == 0);
  make_key(40, key);
  receipt = aula_device_credentials_receipt(key);
  GATEWAY_CHECK(receipt != NULL && json_integer_value(json_object_get(receipt, "revision")) == 41);
  json_decref(receipt);
  GATEWAY_CHECK(strcmp(update(1, 1), "revision_conflict") == 0);
  GATEWAY_CHECK(status_revision() == 41);
  aula_device_credentials_close();
}

static void test_v1_upgrade(int directory) {
  static const char v1[] = "{\"credentials\":{\"password\":\"secret\",\"username\":\"admin\"},"
      "\"receipts\":[{\"fingerprint\":\"%064x\",\"key\":\"%064x\"},"
      "{\"fingerprint\":\"%064x\",\"key\":\"%064x\"}],\"revision\":3,\"version\":1}";
  char body[512];
  json_t *document, *receipts;
  json_error_t error;
  int fd = openat(directory, FILE_NAME, O_WRONLY | O_CREAT | O_EXCL, 0600);
  GATEWAY_CHECK(fd >= 0);
  if (fd < 0) return;
  snprintf(body, sizeof(body), v1, 1U, 1U, 2U, 2U);
  GATEWAY_CHECK(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
  close(fd);
  GATEWAY_CHECK(aula_device_credentials_open(directory));
  GATEWAY_CHECK(status_revision() == 3);
  GATEWAY_CHECK(strcmp(update(3, 3), "succeeded") == 0);
  aula_device_credentials_close();
  fd = openat(directory, FILE_NAME, O_RDONLY);
  GATEWAY_CHECK(fd >= 0);
  if (fd < 0) return;
  document = json_loadfd(fd, 0, &error);
  close(fd);
  GATEWAY_CHECK(document != NULL);
  GATEWAY_CHECK(json_integer_value(json_object_get(document, "version")) == 2);
  receipts = json_object_get(document, "receipts");
  GATEWAY_CHECK(json_array_size(receipts) == 3);
  GATEWAY_CHECK(json_integer_value(json_object_get(json_array_get(receipts, 0), "revision")) == 2);
  GATEWAY_CHECK(json_integer_value(json_object_get(json_array_get(receipts, 2), "revision")) == 4);
  json_decref(document);
  GATEWAY_CHECK(aula_device_credentials_open(directory));
  GATEWAY_CHECK(status_revision() == 4);
  aula_device_credentials_close();
}

/* A blocked pending file fails the save; once it is gone the next call heals without a reopen. */
static void test_recovery_after_failed_save(int directory) {
  GATEWAY_CHECK(aula_device_credentials_open(directory));
  GATEWAY_CHECK(mkdirat(directory, "oem-credentials.pending", 0700) == 0);
  GATEWAY_CHECK(strcmp(update(1, 1), "persistence_uncertain") == 0);
  GATEWAY_CHECK(strcmp(update(1, 1), "unavailable") == 0);
  GATEWAY_CHECK(status_revision() == -1);
  GATEWAY_CHECK(unlinkat(directory, "oem-credentials.pending", AT_REMOVEDIR) == 0);
  GATEWAY_CHECK(status_revision() == 1);
  GATEWAY_CHECK(strcmp(update(1, 1), "succeeded") == 0);
  GATEWAY_CHECK(status_revision() == 2);
  aula_device_credentials_close();
}

int main(void) {
  const char *base = getenv("TMPDIR");
  char path[1024];
  int directory;
  if (base == NULL || *base == '\0') base = "/tmp";
  snprintf(path, sizeof(path), "%s/aula-credentials-XXXXXX", base);
  if (mkdtemp(path) == NULL) { perror("mkdtemp"); return 1; }
  directory = open(path, O_RDONLY | O_DIRECTORY);
  GATEWAY_CHECK(directory >= 0);
  if (directory >= 0) {
    test_eviction(directory);
    unlinkat(directory, FILE_NAME, 0);
    test_v1_upgrade(directory);
    unlinkat(directory, FILE_NAME, 0);
    test_recovery_after_failed_save(directory);
    unlinkat(directory, FILE_NAME, 0);
    close(directory);
  }
  rmdir(path);
  return gateway_test_result("test_device_credentials");
}
