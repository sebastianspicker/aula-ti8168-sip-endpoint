#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "credentials_store.h"
#include "credentials.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int state_directory = -1;
static int state_healthy;
static json_t *state;

static int hex_digest(const char *text) {
  return text != NULL && strlen(text) == 64 && strspn(text, "0123456789abcdef") == 64;
}

static int receipts_valid(json_t *receipts) {
  size_t i, j;
  json_t *entry;
  if (!json_is_array(receipts) || json_array_size(receipts) > 32) return 0;
  json_array_foreach(receipts, i, entry) {
    const char *key = json_string_value(json_object_get(entry, "key"));
    if (!json_is_object(entry) || json_object_size(entry) != 2 || !hex_digest(key) ||
        !hex_digest(json_string_value(json_object_get(entry, "fingerprint")))) return 0;
    for (j = 0; j < i; ++j)
      if (strcmp(key, json_string_value(json_object_get(json_array_get(receipts, j), "key"))) == 0) return 0;
  }
  return 1;
}

static int credentials_state_valid(json_t *document) {
  json_t *credentials = json_object_get(document, "credentials");
  json_t *receipts = json_object_get(document, "receipts");
  json_t *version = json_object_get(document, "version");
  json_t *revision = json_object_get(document, "revision");
  int present = json_is_object(credentials);
  if (!json_is_object(document) || json_object_size(document) != 4 ||
      !json_is_integer(version) || json_integer_value(version) != 1 ||
      !json_is_integer(revision) || !receipts_valid(receipts)) return 0;
  if (json_integer_value(revision) != (json_int_t)json_array_size(receipts) + 1) return 0;
  if (!present) return json_is_null(credentials) && json_array_size(receipts) == 0;
  return json_array_size(receipts) > 0 && json_object_size(credentials) == 2 &&
      ls200_device_credential_text(json_object_get(credentials, "username"), 64) &&
      ls200_device_credential_text(json_object_get(credentials, "password"), 128);
}

static int credentials_file_valid(int fd) {
  struct stat info;
  return fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
      (info.st_mode & 0077) == 0 && info.st_nlink == 1 &&
      info.st_size >= 0 && info.st_size <= 16384;
}

void ls200_device_credentials_close(void) {
  json_decref(state);
  state = NULL;
  if (state_directory >= 0) close(state_directory);
  state_directory = -1;
  state_healthy = 0;
}

static int recover_credentials_pending(int directory) {
  int fd = openat(directory, "oem-credentials.pending", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  int okay;
  if (fd < 0) return errno == ENOENT;
  okay = credentials_file_valid(fd);
  close(fd);
  return okay && unlinkat(directory, "oem-credentials.pending", 0) == 0 && fsync(directory) == 0;
}

int ls200_device_credentials_open(int directory) {
  struct stat info;
  json_error_t error;
  int fd;
  ls200_device_credentials_close();
  if (fstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077) != 0) return 0;
  state_directory = fcntl(directory, F_DUPFD_CLOEXEC, 0);
  if (state_directory < 0) return 0;
  fd = openat(directory, "oem-credentials.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0 && errno == ENOENT)
    state = json_pack("{s:i,s:i,s:n,s:[]}", "version", 1, "revision", 1, "credentials", "receipts");
  else if (fd >= 0 && credentials_file_valid(fd))
    state = json_loadfd(fd, JSON_REJECT_DUPLICATES, &error);
  if (fd >= 0) close(fd);
  state_healthy = credentials_state_valid(state) && recover_credentials_pending(directory);
  return state_healthy;
}

static int save_credentials(json_t *document) {
  char *body = json_dumps(document, JSON_COMPACT | JSON_SORT_KEYS);
  size_t offset = 0, length = body == NULL ? 0 : strlen(body);
  int fd, okay = 0;
  state_healthy = 0;
  if (!length) return 0;
  /* A stale temporary file is never overwritten or followed. It must be
   * validated and removed on a later startup before accepting another write. */
  fd = openat(state_directory, "oem-credentials.pending", O_WRONLY | O_CREAT |
      O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) goto done;
  while (offset < length) {
    ssize_t count = write(fd, body + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += (size_t)count;
  }
  okay = offset == length && fsync(fd) == 0;
  if (close(fd) != 0) okay = 0;
  if (okay) okay = renameat(state_directory, "oem-credentials.pending",
      state_directory, "oem-credentials.json") == 0 && fsync(state_directory) == 0;
done:
  OPENSSL_cleanse(body, length);
  free(body);
  state_healthy = okay;
  return okay;
}

static int arguments_digest(json_t *arguments, char output[65]) {
  unsigned char digest[32];
  unsigned size;
  char *encoded = json_dumps(arguments, JSON_COMPACT | JSON_SORT_KEYS);
  int okay;
  if (encoded == NULL) return 0;
  okay = EVP_Digest(encoded, strlen(encoded), digest, &size, EVP_sha256(), NULL) == 1 && size == 32;
  OPENSSL_cleanse(encoded, strlen(encoded));
  free(encoded);
  if (okay) for (unsigned i = 0; i < 32; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
  OPENSSL_cleanse(digest, sizeof(digest));
  return okay;
}

static const char *credential_duplicate(const char *key, const char *fingerprint) {
  size_t index;
  json_t *receipt;
  json_array_foreach(json_object_get(state, "receipts"), index, receipt) {
    if (strcmp(key, json_string_value(json_object_get(receipt, "key")))) continue;
    return strcmp(fingerprint, json_string_value(json_object_get(receipt, "fingerprint"))) == 0
        ? "duplicate" : "idempotency_conflict";
  }
  return NULL;
}

const char *ls200_device_credentials_update(json_t *arguments, const char *key) {
  char fingerprint[65];
  const char *duplicate;
  json_t *next, *credentials, *receipt;
  json_int_t revision;
  int okay;
  if (!state_healthy) return "unavailable";
  if (!ls200_device_credentials_schema(arguments) || !hex_digest(key)) return "invalid";
  if (!arguments_digest(arguments, fingerprint)) return "unavailable";
  duplicate = credential_duplicate(key, fingerprint);
  if (duplicate != NULL) return duplicate;
  revision = json_integer_value(json_object_get(state, "revision"));
  if (revision != json_integer_value(json_object_get(arguments, "revision"))) return "revision_conflict";
  if (json_array_size(json_object_get(state, "receipts")) == 32) return "capacity";
  next = json_deep_copy(state);
  credentials = json_pack("{s:O,s:O}", "username", json_object_get(arguments, "username"),
      "password", json_object_get(arguments, "password"));
  receipt = json_pack("{s:s,s:s}", "key", key, "fingerprint", fingerprint);
  okay = next != NULL && credentials != NULL && receipt != NULL;
  if (okay) okay = json_object_set(next, "credentials", credentials) == 0 &&
      json_object_set_new(next, "revision", json_integer(revision + 1)) == 0 &&
      json_array_append(json_object_get(next, "receipts"), receipt) == 0;
  json_decref(credentials); json_decref(receipt);
  if (!okay) { json_decref(next); return "unavailable"; }
  if (!save_credentials(next)) { json_decref(next); return "persistence_uncertain"; }
  json_decref(state); state = next;
  return "succeeded";
}

json_t *ls200_device_credentials_status(const char *connection) {
  if (!state_healthy) return NULL;
  return json_pack("{s:O,s:b,s:s}", "revision", json_object_get(state, "revision"),
      "credentials_present", json_is_object(json_object_get(state, "credentials")),
      "connection", connection);
}

json_t *ls200_device_credentials_receipt(const char *key) {
  json_t *receipt;
  size_t index;
  if (!state_healthy || key == NULL) return NULL;
  json_array_foreach(json_object_get(state, "receipts"), index, receipt) {
    if (strcmp(key, json_string_value(json_object_get(receipt, "key"))) != 0) continue;
    /* Receipts are append-only and each accepted write advances once. */
    return json_pack("{s:i,s:b,s:s}", "revision", (int)index + 2,
        "credentials_present", 1, "connection", "unknown");
  }
  return NULL;
}

char *ls200_device_credentials_login(void) {
  json_t *credentials = json_object_get(state, "credentials");
  char plain[194], encoded[261];
  char *result = NULL;
  json_t *body;
  int length;
  if (!state_healthy || !json_is_object(credentials)) return NULL;
  length = snprintf(plain, sizeof(plain), "%s:%s",
      json_string_value(json_object_get(credentials, "username")),
      json_string_value(json_object_get(credentials, "password")));
  if (length > 0 && length < (int)sizeof(plain)) {
    EVP_EncodeBlock((unsigned char *)encoded, (unsigned char *)plain, length);
    body = json_pack("{s:s,s:i}", "authorization", encoded, "tab_id", 0);
    result = json_dumps(body, JSON_COMPACT);
    json_decref(body);
  }
  OPENSSL_cleanse(plain, sizeof(plain));
  OPENSSL_cleanse(encoded, sizeof(encoded));
  return result;
}
