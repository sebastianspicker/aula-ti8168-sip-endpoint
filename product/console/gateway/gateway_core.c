#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif defined(__linux__)
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(__APPLE__)
extern int getpeereid(int descriptor, uid_t *uid, gid_t *gid);
#endif

int gateway_state_lock(ls200_gateway *gateway) {
  return gateway != NULL && gateway->synchronization_ready != 0 &&
      pthread_mutex_lock(&gateway->state_mutex) == 0;
}

void gateway_state_unlock(ls200_gateway *gateway) {
  (void)pthread_mutex_unlock(&gateway->state_mutex);
}

static void *secure_json_malloc(size_t size) {
  return malloc(size);
}

void gateway_secure_json_free(void *memory) {
  size_t size;
  if (memory == NULL) return;
#if defined(__APPLE__)
  size = malloc_size(memory);
#elif defined(__linux__)
  size = malloc_usable_size(memory);
#else
  size = 0U;
#endif
  OPENSSL_cleanse(memory, size);
  free(memory);
}

void gateway_install_secure_json_allocator(void) {
  /* Gateway initialization precedes FastCGI worker creation. Reinstalling
   * these identical process-global callbacks is safe in unit fixtures too. */
  json_set_alloc_funcs(secure_json_malloc, gateway_secure_json_free);
}

int ls200_gateway_parse_content_length(const char *text, size_t maximum, size_t *length_out) {
  char *end = NULL;
  unsigned long value;
  if (length_out == NULL) return 0;
  if (text == NULL || text[0] == '\0') {
    *length_out = 0U;
    return 1;
  }
  errno = 0;
  value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > maximum) return 0;
  *length_out = (size_t)value;
  return 1;
}

int gateway_is_safe_username(const char *value) {
  size_t index;
  if (value == NULL || value[0] == '\0' || strlen(value) > 32U) return 0;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isalnum((unsigned char)value[index]) && value[index] != '_' && value[index] != '-') return 0;
  return 1;
}

int gateway_is_safe_idempotency_key(const char *value) {
  size_t index;
  if (value == NULL || strlen(value) < 16U || strlen(value) > 80U) return 0;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isalnum((unsigned char)value[index]) && value[index] != '_' && value[index] != '-') return 0;
  return 1;
}

int gateway_secure_equal(const void *left, const void *right, size_t length) {
  return left != NULL && right != NULL && CRYPTO_memcmp(left, right, length) == 0;
}

int gateway_hex_encode(const uint8_t *input, size_t length, char *output, size_t capacity) {
  static const char digits[] = "0123456789abcdef";
  size_t index;
  if (input == NULL || output == NULL || capacity < length * 2U + 1U) return 0;
  for (index = 0U; index < length; ++index) {
    output[index * 2U] = digits[input[index] >> 4U];
    output[index * 2U + 1U] = digits[input[index] & 15U];
  }
  output[length * 2U] = '\0';
  return 1;
}

int gateway_hex_decode_32(const char *input, uint8_t output[LS200_GATEWAY_HASH_BYTES]) {
  size_t index;
  if (input == NULL || strlen(input) != LS200_GATEWAY_HASH_BYTES * 2U) return 0;
  for (index = 0U; index < LS200_GATEWAY_HASH_BYTES; ++index) {
    int high = isdigit((unsigned char)input[index * 2U]) ? input[index * 2U] - '0' :
        (input[index * 2U] >= 'a' && input[index * 2U] <= 'f' ? input[index * 2U] - 'a' + 10 : -1);
    int low = isdigit((unsigned char)input[index * 2U + 1U]) ? input[index * 2U + 1U] - '0' :
        (input[index * 2U + 1U] >= 'a' && input[index * 2U + 1U] <= 'f' ? input[index * 2U + 1U] - 'a' + 10 : -1);
    if (high < 0 || low < 0) return 0;
    output[index] = (uint8_t)((high << 4U) | low);
  }
  return 1;
}

int gateway_hex_decode(const char *input, uint8_t *output, size_t output_length) {
  size_t index;
  if (input == NULL || output == NULL || strlen(input) != output_length * 2U) return 0;
  for (index = 0U; index < output_length; ++index) {
    int high = isdigit((unsigned char)input[index * 2U]) ? input[index * 2U] - '0' :
        (input[index * 2U] >= 'a' && input[index * 2U] <= 'f' ? input[index * 2U] - 'a' + 10 : -1);
    int low = isdigit((unsigned char)input[index * 2U + 1U]) ? input[index * 2U + 1U] - '0' :
        (input[index * 2U + 1U] >= 'a' && input[index * 2U + 1U] <= 'f' ? input[index * 2U + 1U] - 'a' + 10 : -1);
    if (high < 0 || low < 0) return 0;
    output[index] = (uint8_t)((high << 4U) | low);
  }
  return 1;
}

int gateway_sha256(const uint8_t *input, size_t input_length, uint8_t output[LS200_GATEWAY_HASH_BYTES]) {
  unsigned int output_length = 0U;
  return EVP_Digest(input, input_length, output, &output_length, EVP_sha256(), NULL) == 1 &&
      output_length == LS200_GATEWAY_HASH_BYTES;
}

int gateway_password_hash(const char *password, const uint8_t salt[LS200_GATEWAY_SALT_BYTES],
                          uint8_t output[LS200_GATEWAY_HASH_BYTES]) {
  if (password == NULL || strlen(password) < 12U || strlen(password) > 256U) return 0;
  return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, LS200_GATEWAY_SALT_BYTES,
                           LS200_GATEWAY_PBKDF2_ITERATIONS, EVP_sha256(),
                           LS200_GATEWAY_HASH_BYTES, output) == 1;
}

int gateway_password_hash_candidate(const char *password, const uint8_t salt[LS200_GATEWAY_SALT_BYTES],
                                    uint8_t output[LS200_GATEWAY_HASH_BYTES]) {
  if (password == NULL || strlen(password) > 256U) return 0;
  return PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, LS200_GATEWAY_SALT_BYTES,
                           LS200_GATEWAY_PBKDF2_ITERATIONS, EVP_sha256(),
                           LS200_GATEWAY_HASH_BYTES, output) == 1;
}

void gateway_write_error(ls200_gateway_response *response, unsigned int status,
                         const char *code, const char *message) {
  response->status = status;
  (void)snprintf(response->content_type, sizeof(response->content_type), "application/json");
  (void)snprintf(response->body, sizeof(response->body),
                 "{\"revision\":%u,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                 LS200_GATEWAY_API_REVISION, code, message);
}

void gateway_write_rate_limited(ls200_gateway_response *response, unsigned int retry_after) {
  gateway_write_error(response, 429U, "AUTH_RATE_LIMITED", "try again later");
  response->retry_after = retry_after == 0U ? 1U : retry_after;
}

void gateway_write_success(ls200_gateway_response *response, unsigned int status, const char *data) {
  response->status = status;
  (void)snprintf(response->content_type, sizeof(response->content_type), "application/json");
  (void)snprintf(response->body, sizeof(response->body),
                 "{\"revision\":%u,\"ok\":true,\"data\":%s}",
                 LS200_GATEWAY_API_REVISION, data == NULL ? "{}" : data);
}

int gateway_json_object_exact(json_t *object, const char *const *keys, size_t key_count) {
  const char *key;
  json_t *value;
  void *iterator;
  size_t index;
  if (!json_is_object(object) || json_object_size(object) != key_count) return 0;
  iterator = json_object_iter(object);
  while (iterator != NULL) {
    key = json_object_iter_key(iterator);
    value = json_object_iter_value(iterator);
    (void)value;
    for (index = 0U; index < key_count && strcmp(key, keys[index]) != 0; ++index) {}
    if (index == key_count) return 0;
    iterator = json_object_iter_next(object, iterator);
  }
  return 1;
}

json_t *gateway_parse_json(const char *body, json_error_t *error) {
  if (body == NULL || strlen(body) > 1024U) return NULL;
  return json_loads(body, JSON_REJECT_DUPLICATES, error);
}

int gateway_safe_absolute_path(const char *path) {
  return path != NULL && path[0] == '/' && path[1] != '\0' && strstr(path, "/../") == NULL &&
      strcmp(path + strlen(path) - (strlen(path) >= 3U ? 3U : 0U), "/..") != 0;
}

static int directory_is_safe(const struct stat *details) {
  return details != NULL && S_ISDIR(details->st_mode) &&
      ((details->st_uid == 0U || details->st_uid == geteuid()) &&
       ((details->st_mode & 0022U) == 0U || (details->st_mode & S_ISVTX) != 0U));
}

static int path_component_is_safe(const char *component) {
  return component != NULL && component[0] != '\0' &&
      strcmp(component, ".") != 0 && strcmp(component, "..") != 0;
}

static int descriptor_is_safe_directory(int descriptor) {
  struct stat details;
  return fstat(descriptor, &details) == 0 && directory_is_safe(&details);
}

static int open_verified_child_directory(int directory, const char *name) {
  struct stat expected;
  struct stat actual;
  int child;
  if (!descriptor_is_safe_directory(directory)) {
    (void)fprintf(stderr, "gateway storage: unsafe parent before component %s\n", name);
    return -1;
  }
  if (fstatat(directory, name, &expected, AT_SYMLINK_NOFOLLOW) != 0) {
    (void)fprintf(stderr, "gateway storage: component metadata failed name=%s errno=%d\n",
                  name, errno);
    return -1;
  }
  if (!directory_is_safe(&expected)) {
    (void)fprintf(stderr,
        "gateway storage: unsafe component name=%s owner=%lu mode=%04o directory=%d\n",
        name, (unsigned long)expected.st_uid,
        (unsigned int)(expected.st_mode & 07777U), S_ISDIR(expected.st_mode) ? 1 : 0);
    return -1;
  }
  child = openat(directory, name, O_RDONLY | O_DIRECTORY | LS200_O_NOFOLLOW);
  if (child < 0) {
    (void)fprintf(stderr, "gateway storage: component open failed name=%s errno=%d\n",
                  name, errno);
    return -1;
  }
  if (fstat(child, &actual) != 0 || actual.st_dev != expected.st_dev ||
      actual.st_ino != expected.st_ino) {
    (void)fprintf(stderr, "gateway storage: component identity changed name=%s\n", name);
    (void)close(child);
    return -1;
  }
  return child;
}

/* Walk each ancestor by descriptor so later pathname replacement cannot change
 * the object whose metadata we validate. Sticky system directories are safe
 * ancestors for per-user test and deployment state. */
int gateway_open_parent_directory(const char *path, int *parent_out, char name[NAME_MAX + 1U]) {
  char copy[512];
  char *cursor;
  char *next;
  int directory;
  if (!gateway_safe_absolute_path(path) || strlen(path) >= sizeof(copy) ||
      parent_out == NULL || name == NULL) return 0;
  if (strncmp(path, "/run/", 5U) == 0) {
    (void)snprintf(copy, sizeof(copy), "%s", path + 5U);
    directory = open("/run", O_RDONLY | O_DIRECTORY | LS200_O_NOFOLLOW);
  } else {
    (void)snprintf(copy, sizeof(copy), "%s", path + 1U);
    directory = open("/", O_RDONLY | O_DIRECTORY);
  }
  if (directory < 0) return 0;
  if (!descriptor_is_safe_directory(directory)) {
    (void)fputs("gateway storage: selected trust anchor is unsafe\n", stderr);
    (void)close(directory);
    return 0;
  }
  cursor = copy;
  while ((next = strchr(cursor, '/')) != NULL) {
    int child;
    *next = '\0';
    if (!path_component_is_safe(cursor)) { (void)close(directory); return 0; }
    child = open_verified_child_directory(directory, cursor);
    (void)close(directory);
    if (child < 0) return 0;
    directory = child;
    cursor = next + 1U;
  }
  if (!path_component_is_safe(cursor) || strlen(cursor) > NAME_MAX ||
      !descriptor_is_safe_directory(directory)) { (void)close(directory); return 0; }
  (void)snprintf(name, NAME_MAX + 1U, "%s", cursor);
  *parent_out = directory;
  return 1;
}

int gateway_open_verified_regular(const char *path, int missing_is_ok, int *descriptor_out) {
  char name[NAME_MAX + 1U];
  struct stat details;
  struct stat expected;
  int parent;
  int descriptor;
  if (!gateway_open_parent_directory(path, &parent, name)) {
    (void)fputs("gateway storage: parent walk failed\n", stderr);
    return 0;
  }
  if (fstatat(parent, name, &expected, AT_SYMLINK_NOFOLLOW) != 0) {
    int failure = errno;
    (void)close(parent);
    if (missing_is_ok && failure == ENOENT) return 2;
    (void)fprintf(stderr, "gateway storage: final metadata failed errno=%d\n", failure);
    return 0;
  }
  descriptor = openat(parent, name, O_RDONLY | LS200_O_NOFOLLOW);
  (void)close(parent);
  if (descriptor < 0) {
    (void)fprintf(stderr, "gateway storage: final open failed errno=%d\n", errno);
    return 0;
  }
  if (fstat(descriptor, &details) != 0) {
    (void)fprintf(stderr, "gateway storage: final fstat failed errno=%d\n", errno);
    (void)close(descriptor);
    return 0;
  }
  if (!S_ISREG(details.st_mode) || details.st_uid != geteuid() ||
      (details.st_mode & 0077U) != 0U || details.st_nlink != 1 ||
      details.st_dev != expected.st_dev || details.st_ino != expected.st_ino) {
    (void)fprintf(stderr,
        "gateway storage: final verification failed euid=%lu owner=%lu mode=%04o links=%lu regular=%d same_object=%d\n",
        (unsigned long)geteuid(), (unsigned long)details.st_uid,
        (unsigned int)(details.st_mode & 07777U), (unsigned long)details.st_nlink,
        S_ISREG(details.st_mode) ? 1 : 0,
        details.st_dev == expected.st_dev && details.st_ino == expected.st_ino);
    (void)close(descriptor);
    return 0;
  }
  *descriptor_out = descriptor;
  return 1;
}

int gateway_sync_account_file(int descriptor) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
  /* APFS/HFS may require the stronger request before ordinary fsync. */
  if (fcntl(descriptor, F_FULLFSYNC) == 0) return 1;
#endif
  return fsync(descriptor) == 0;
}

int gateway_sync_parent_directory(int descriptor) {
  if (fsync(descriptor) == 0) return 1;
#if defined(__APPLE__)
  /* Darwin does not implement directory fsync. The renamed file itself was
   * fully synced above; never extend this capability exception to Linux. */
  return errno == EINVAL || errno == ENOTSUP;
#else
  return 0;
#endif
}
