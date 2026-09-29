#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "config_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static aula_status validate_owned_parent(const char *path) {
  struct stat parent_details;
  char parent[PATH_MAX];
  char *slash;
  if (path == NULL || path[0] != '/' ||
      aula_config_has_control_or_parent_segment(path)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  if (strlen(path) >= sizeof(parent)) return AULA_STATUS_LIMIT_EXCEEDED;
  (void)snprintf(parent, sizeof(parent), "%s", path);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent || slash[1] == '\0') {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  *slash = '\0';
  if (lstat(parent, &parent_details) != 0 ||
      !S_ISDIR(parent_details.st_mode) ||
      (parent_details.st_mode & 0022U) != 0U ||
      (parent_details.st_uid != 0U && parent_details.st_uid != geteuid())) {
    return AULA_STATUS_PERMISSION_DENIED;
  }
  return AULA_STATUS_OK;
}

static int owned_regular_descriptor_is_valid(int descriptor, int secret) {
  struct stat details;
  return fstat(descriptor, &details) == 0 && S_ISREG(details.st_mode) &&
         details.st_nlink == 1 && details.st_uid == geteuid() &&
         (details.st_mode & 0022U) == 0U &&
         (!secret || (details.st_mode & 0077U) == 0U);
}

aula_status aula_config_validate_runtime_settings_path(const char *path) {
  struct stat details;
  int descriptor;
  aula_status status;
  if (path == NULL || path[0] == '\0') return AULA_STATUS_OK;
  status = validate_owned_parent(path);
  if (status != AULA_STATUS_OK) return status;
  if (lstat(path, &details) != 0) {
    return errno == ENOENT ? AULA_STATUS_OK : AULA_STATUS_IO_ERROR;
  }
  if (S_ISLNK(details.st_mode)) return AULA_STATUS_SECURITY_ERROR;
  descriptor = openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return errno == ELOOP ? AULA_STATUS_SECURITY_ERROR : AULA_STATUS_IO_ERROR;
  }
  if (!owned_regular_descriptor_is_valid(descriptor, 1) ||
      fstat(descriptor, &details) != 0 ||
      (details.st_mode & 0777U) != 0600U) {
    (void)close(descriptor);
    return AULA_STATUS_PERMISSION_DENIED;
  }
  (void)close(descriptor);
  return AULA_STATUS_OK;
}

aula_status aula_config_open_owned_regular_file(const char *path, int secret,
                                                   int *out_descriptor) {
  int descriptor;
  aula_status status;
  if (out_descriptor == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_descriptor = -1;
  status = validate_owned_parent(path);
  if (status != AULA_STATUS_OK) return status;
  descriptor = openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return errno == ELOOP ? AULA_STATUS_SECURITY_ERROR : AULA_STATUS_IO_ERROR;
  }
  if (!owned_regular_descriptor_is_valid(descriptor, secret)) {
    (void)close(descriptor);
    return AULA_STATUS_PERMISSION_DENIED;
  }
  *out_descriptor = descriptor;
  return AULA_STATUS_OK;
}

static void secure_zero(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

static aula_status read_secret_bytes(int descriptor, aula_mutable_bytes *output,
                                      size_t *out_used) {
  size_t used = 0U;
  while (used < output->capacity) {
    ssize_t count = read(descriptor, output->data + used, output->capacity - used);
    if (count > 0) {
      used += (size_t)count;
      continue;
    }
    if (count == 0) break;
    if (errno != EINTR) return AULA_STATUS_IO_ERROR;
  }
  *out_used = used;
  return AULA_STATUS_OK;
}

static aula_status check_secret_capacity(int descriptor, size_t used,
                                          size_t capacity) {
  unsigned char extra;
  ssize_t count;
  if (used != capacity) return AULA_STATUS_OK;
  do {
    count = read(descriptor, &extra, 1U);
  } while (count < 0 && errno == EINTR);
  return count == 0 ? AULA_STATUS_OK : AULA_STATUS_LIMIT_EXCEEDED;
}

static void trim_secret_line_ending(aula_mutable_bytes *output, size_t *used) {
  if (*used > 0U && output->data[*used - 1U] == '\n') {
    --*used;
    if (*used > 0U && output->data[*used - 1U] == '\r') --*used;
  }
}

aula_status aula_config_read_secret_file(const char *path,
                                           aula_mutable_bytes *output) {
  int descriptor;
  size_t used = 0U;
  aula_status status;
  if (path == NULL || output == NULL || output->data == NULL ||
      output->capacity == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  output->length = 0U;
  status = aula_config_open_owned_regular_file(path, 1, &descriptor);
  if (status != AULA_STATUS_OK) return status;
  status = read_secret_bytes(descriptor, output, &used);
  if (status == AULA_STATUS_OK) {
    status = check_secret_capacity(descriptor, used, output->capacity);
  }
  (void)close(descriptor);
  if (status == AULA_STATUS_OK) trim_secret_line_ending(output, &used);
  if (status != AULA_STATUS_OK || used == 0U ||
      memchr(output->data, '\0', used) != NULL) {
    secure_zero(output->data, output->capacity);
    output->length = 0U;
    return status == AULA_STATUS_OK ? AULA_STATUS_INVALID_DATA : status;
  }
  output->length = used;
  return AULA_STATUS_OK;
}

static aula_status write_all(int descriptor, aula_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    ssize_t written = write(descriptor, input.data + offset,
                            input.length - offset);
    if (written > 0) {
      offset += (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    return AULA_STATUS_IO_ERROR;
  }
  return AULA_STATUS_OK;
}

static int secret_replace_input_is_valid(const char *path, aula_bytes secret) {
  return path != NULL && secret.data != NULL && secret.length > 0U &&
         secret.length <= 256U &&
         memchr(secret.data, '\0', secret.length) == NULL &&
         memchr(secret.data, '\r', secret.length) == NULL &&
         memchr(secret.data, '\n', secret.length) == NULL;
}

static int existing_secret_is_valid(int parent_descriptor, const char *name) {
  struct stat parent_details;
  struct stat existing;
  return fstat(parent_descriptor, &parent_details) == 0 &&
         S_ISDIR(parent_details.st_mode) &&
         (parent_details.st_uid == 0U || parent_details.st_uid == geteuid()) &&
         (parent_details.st_mode & 0022U) == 0U &&
         fstatat(parent_descriptor, name, &existing, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(existing.st_mode) && existing.st_nlink == 1 &&
         existing.st_uid == geteuid() && (existing.st_mode & 0077U) == 0U;
}

static aula_status open_secret_parent(const char *path, char *parent,
                                       size_t parent_capacity, char **out_name,
                                       int *out_descriptor) {
  char *name;
  aula_status status = validate_owned_parent(path);
  if (status != AULA_STATUS_OK) return status;
  if (strlen(path) >= parent_capacity) return AULA_STATUS_INVALID_ARGUMENT;
  (void)snprintf(parent, parent_capacity, "%s", path);
  name = strrchr(parent, '/');
  if (name == NULL || name == parent || name[1] == '\0') {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  *name++ = '\0';
  *out_descriptor = open(parent, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (*out_descriptor < 0 || !existing_secret_is_valid(*out_descriptor, name)) {
    if (*out_descriptor >= 0) (void)close(*out_descriptor);
    *out_descriptor = -1;
    return AULA_STATUS_PERMISSION_DENIED;
  }
  *out_name = name;
  return AULA_STATUS_OK;
}

static aula_status open_secret_temporary(int parent_descriptor, char *temporary,
                                          size_t temporary_capacity,
                                          int *out_descriptor) {
  static unsigned long sequence;
  unsigned int attempt;
  for (attempt = 0U; attempt < 16U; ++attempt) {
    ++sequence;
    if (snprintf(temporary, temporary_capacity, ".aula-secret-%ld-%lu",
                 (long)getpid(), sequence) <= 0) {
      break;
    }
    *out_descriptor = openat(parent_descriptor, temporary,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                             0600);
    if (*out_descriptor >= 0 || errno != EEXIST) break;
  }
  return *out_descriptor < 0 ? AULA_STATUS_IO_ERROR : AULA_STATUS_OK;
}

static int created_secret_is_valid(int descriptor) {
  struct stat created;
  return fstat(descriptor, &created) == 0 && S_ISREG(created.st_mode) &&
         created.st_nlink == 1 && created.st_uid == geteuid() &&
         (created.st_mode & 0077U) == 0U;
}

static aula_status sync_secret_parent(int descriptor) {
  return fsync(descriptor) == 0 ? AULA_STATUS_OK : AULA_STATUS_IO_ERROR;
}

aula_status aula_config_replace_secret_file(const char *path,
                                              aula_bytes secret) {
  char parent[PATH_MAX];
  char temporary[96];
  char *name;
  int parent_fd = -1;
  int temporary_fd = -1;
  aula_status status;
  if (!secret_replace_input_is_valid(path, secret)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (strlen(path) >= sizeof(parent)) return AULA_STATUS_INVALID_ARGUMENT;
  status = open_secret_parent(path, parent, sizeof(parent), &name, &parent_fd);
  if (status != AULA_STATUS_OK) return status;
  status = open_secret_temporary(parent_fd, temporary, sizeof(temporary),
                                 &temporary_fd);
  if (status != AULA_STATUS_OK) {
    (void)close(parent_fd);
    return status;
  }
  status = write_all(temporary_fd, secret);
  if (status == AULA_STATUS_OK &&
      (fsync(temporary_fd) != 0 || !created_secret_is_valid(temporary_fd))) {
    status = AULA_STATUS_IO_ERROR;
  }
  if (close(temporary_fd) != 0 && status == AULA_STATUS_OK) {
    status = AULA_STATUS_IO_ERROR;
  }
  temporary_fd = -1;
  if (status == AULA_STATUS_OK &&
      renameat(parent_fd, temporary, parent_fd, name) != 0) {
    status = AULA_STATUS_IO_ERROR;
  }
  if (status == AULA_STATUS_OK) {
    status = sync_secret_parent(parent_fd);
    if (status != AULA_STATUS_OK) {
      /* renameat is already the commit point.  Do not attempt an unreliable
       * rollback, and make the committed-but-not-crash-durable outcome
       * explicit to control callers. */
      status = AULA_STATUS_PERSISTENCE_UNCERTAIN;
    }
  }
  if (status != AULA_STATUS_OK) (void)unlinkat(parent_fd, temporary, 0);
  (void)close(parent_fd);
  return status;
}

aula_status aula_config_validate_secret_file(const char *path) {
  unsigned char buffer[512];
  size_t total = 0U;
  int descriptor;
  aula_status status = aula_config_open_owned_regular_file(path, 1, &descriptor);
  if (status != AULA_STATUS_OK) return status;
  for (;;) {
    ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count > 0) {
      total += (size_t)count;
      if (total > AULA_CONFIG_FILE_BYTES) {
        secure_zero(buffer, sizeof(buffer));
        (void)close(descriptor);
        return AULA_STATUS_LIMIT_EXCEEDED;
      }
      continue;
    }
    if (count == 0) break;
    if (errno == EINTR) continue;
    secure_zero(buffer, sizeof(buffer));
    (void)close(descriptor);
    return AULA_STATUS_IO_ERROR;
  }
  secure_zero(buffer, sizeof(buffer));
  (void)close(descriptor);
  return total == 0U ? AULA_STATUS_CONFIGURATION_ERROR : AULA_STATUS_OK;
}
