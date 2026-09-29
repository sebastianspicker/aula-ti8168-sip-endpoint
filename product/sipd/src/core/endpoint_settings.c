#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "endpoint_internal.h"
#include "config_private.h"
#include "../control/request_decode.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AULA_SETTINGS_STATE_BYTES 160U

static const char *settings_profile_name(aula_zoom_profile profile) {
  static const char *const names[] = {
      "zoom_direct", "zoom_proxy", "private_lab"};
  return profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB ? names[profile] : NULL;
}

static aula_status settings_encode(uint32_t revision, aula_zoom_profile profile,
                                    int media_managed,
                                    aula_mutable_bytes *output) {
  const char *name = settings_profile_name(profile);
  int length;
  if (output == NULL || output->data == NULL || output->capacity == 0U ||
      revision == 0U || name == NULL ||
      (media_managed != 0 && media_managed != 1)) return AULA_STATUS_INVALID_ARGUMENT;
  length = snprintf((char *)output->data, output->capacity,
      "{\"revision\":%u,\"profile\":\"%s\",\"media\":\"%s\",\"tls\":\"%s\"}",
      revision, name, media_managed != 0 ? "managed" : "disabled",
      "required");
  if (length < 0 || (size_t)length >= output->capacity) return AULA_STATUS_INTERNAL_ERROR;
  output->length = (size_t)length;
  return AULA_STATUS_OK;
}

static int state_file_is_absent(const char *path) {
  struct stat details;
  return lstat(path, &details) != 0 && errno == ENOENT;
}

static int settings_read_input_is_valid(const char *path,
                                        const aula_mutable_bytes *output) {
  return path != NULL && output != NULL && output->data != NULL &&
         output->capacity >= AULA_SETTINGS_STATE_BYTES;
}

static int settings_descriptor_has_valid_size(int descriptor, size_t capacity) {
  struct stat details;
  return fstat(descriptor, &details) == 0 && details.st_size > 0 &&
         (uintmax_t)details.st_size < capacity;
}

static aula_status settings_read_bytes(int descriptor,
                                        aula_mutable_bytes *output,
                                        size_t *out_used) {
  size_t used = 0U;
  while (used < output->capacity) {
    ssize_t count = read(descriptor, output->data + used,
                         output->capacity - used);
    if (count > 0) {
      used += (size_t)count;
    } else if (count == 0) {
      break;
    } else if (errno != EINTR) {
      return AULA_STATUS_IO_ERROR;
    }
  }
  *out_used = used;
  return AULA_STATUS_OK;
}

static aula_status settings_read(const char *path, aula_mutable_bytes *output) {
  int descriptor = -1;
  size_t used = 0U;
  aula_status status;
  if (!settings_read_input_is_valid(path, output)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_config_open_owned_regular_file(path, 1, &descriptor);
  if (status != AULA_STATUS_OK) return status;
  if (!settings_descriptor_has_valid_size(descriptor, output->capacity)) {
    (void)close(descriptor);
    return AULA_STATUS_INVALID_DATA;
  }
  status = settings_read_bytes(descriptor, output, &used);
  if (status != AULA_STATUS_OK) {
    (void)close(descriptor);
    return status;
  }
  if (close(descriptor) != 0) return AULA_STATUS_IO_ERROR;
  if (used == 0U || used == output->capacity ||
      memchr(output->data, '\0', used) != NULL) return AULA_STATUS_INVALID_DATA;
  output->length = used;
  return AULA_STATUS_OK;
}

static int settings_sip_uri_character_is_valid(unsigned char character) {
  return (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
         (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
         (character >= (unsigned char)'0' && character <= (unsigned char)'9') ||
         strchr("-_.~%+:@;=?", (int)character) != NULL;
}

static int settings_sip_uri_is_valid(const char *uri) {
  const char *cursor;
  if (uri == NULL || strncmp(uri, "sip:", 4U) != 0 || uri[4] == '\0') {
    return 0;
  }
  for (cursor = uri + 4; *cursor != '\0'; ++cursor) {
    if (!settings_sip_uri_character_is_valid((unsigned char)*cursor)) return 0;
  }
  return 1;
}

static aula_status settings_validate_tls_baseline(
    const aula_config_view *view) {
  int descriptor;
  aula_status status;
  if (view == NULL || view->enable_tls == 0 ||
      view->transport != AULA_TRANSPORT_TLS || view->tls_ca_file == NULL ||
      view->tls_ca_file[0] == '\0' ||
      view->media_security_policy != REQUIRE_SRTP) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  status = aula_config_open_owned_regular_file(view->tls_ca_file, 0,
                                                 &descriptor);
  if (status != AULA_STATUS_OK) return status;
  (void)close(descriptor);
  return AULA_STATUS_OK;
}

static aula_status settings_validate_public_profile_baseline(
    const aula_endpoint *endpoint, aula_zoom_profile profile) {
  const aula_config_view *view;
  aula_status status;
  if (profile == AULA_ZOOM_PROFILE_PRIVATE_LAB) return AULA_STATUS_OK;
  if (endpoint == NULL || endpoint->view == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  view = endpoint->view;
  status = settings_validate_tls_baseline(view);
  if (status != AULA_STATUS_OK) return status;
  if (profile == AULA_ZOOM_PROFILE_DIRECT_CRC) {
    return view->sip_registrar_uri != NULL && view->sip_registrar_uri[0] == '\0'
               ? AULA_STATUS_OK
               : AULA_STATUS_CONFIGURATION_ERROR;
  }
  return settings_sip_uri_is_valid(view->sip_registrar_uri)
             ? AULA_STATUS_OK
             : AULA_STATUS_CONFIGURATION_ERROR;
}

static aula_status settings_validate_proxy_credentials(
    const aula_endpoint *endpoint) {
  return endpoint->sip_config.auth_username != NULL &&
                 endpoint->sip_config.auth_username[0] != '\0' &&
                 endpoint->sip_config.auth_secret_file != NULL &&
                 endpoint->sip_config.auth_secret_file[0] != '\0'
             ? AULA_STATUS_OK
             : AULA_STATUS_PERMISSION_DENIED;
}

aula_status endpoint_settings_validate_candidate(
    const aula_endpoint *endpoint, aula_zoom_profile profile,
    int media_managed) {
  aula_status status;
  if (endpoint == NULL || profile > AULA_ZOOM_PROFILE_PRIVATE_LAB ||
      (media_managed != 0 && media_managed != 1)) return AULA_STATUS_INVALID_ARGUMENT;
  status = settings_validate_public_profile_baseline(endpoint, profile);
  if (status != AULA_STATUS_OK) return status;
  if (profile == AULA_ZOOM_PROFILE_PROXY_REGISTRATION) {
    status = settings_validate_proxy_credentials(endpoint);
    if (status != AULA_STATUS_OK) return status;
  }
  return endpoint->dialog == NULL ? AULA_STATUS_OK : AULA_STATUS_STATE_ERROR;
}

static aula_status settings_split_parent_path(const char *path, char *parent,
                                                size_t parent_capacity,
                                                char **out_name) {
  char *name;
  if (path == NULL || parent == NULL || out_name == NULL ||
      strlen(path) >= parent_capacity) return AULA_STATUS_INVALID_ARGUMENT;
  (void)snprintf(parent, parent_capacity, "%s", path);
  name = strrchr(parent, '/');
  if (name == NULL || name == parent || name[1] == '\0') {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  *name++ = '\0';
  *out_name = name;
  return AULA_STATUS_OK;
}

static int settings_parent_descriptor_is_trusted(int descriptor) {
  struct stat details;
  return fstat(descriptor, &details) == 0 && S_ISDIR(details.st_mode) &&
         (details.st_mode & 0022U) == 0U &&
         (details.st_uid == 0U || details.st_uid == geteuid());
}

static aula_status settings_open_parent(const char *path, char *parent,
                                         size_t parent_capacity, char **out_name,
                                         int *out_descriptor) {
  aula_status status;
  if (out_descriptor == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_config_validate_runtime_settings_path(path);
  if (status != AULA_STATUS_OK) return status;
  status = settings_split_parent_path(path, parent, parent_capacity, out_name);
  if (status != AULA_STATUS_OK) return status;
  *out_descriptor = open(parent, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (*out_descriptor < 0) return AULA_STATUS_IO_ERROR;
  if (!settings_parent_descriptor_is_trusted(*out_descriptor)) {
    (void)close(*out_descriptor);
    *out_descriptor = -1;
    return AULA_STATUS_PERMISSION_DENIED;
  }
  return AULA_STATUS_OK;
}

static aula_status settings_open_temporary(int parent_descriptor, char *temporary,
                                            size_t temporary_capacity,
                                            int *out_descriptor) {
  static unsigned long sequence;
  unsigned int attempt;
  for (attempt = 0U; attempt < 16U; ++attempt) {
    ++sequence;
    if (snprintf(temporary, temporary_capacity, ".aula-settings-%ld-%lu",
                 (long)getpid(), sequence) <= 0) break;
    *out_descriptor = openat(parent_descriptor, temporary,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (*out_descriptor >= 0 || errno != EEXIST) break;
  }
  return *out_descriptor < 0 ? AULA_STATUS_IO_ERROR : AULA_STATUS_OK;
}

static aula_status settings_write_all(int descriptor, aula_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    ssize_t written = write(descriptor, input.data + offset, input.length - offset);
    if (written > 0) offset += (size_t)written;
    else if (written < 0 && errno == EINTR) continue;
    else return AULA_STATUS_IO_ERROR;
  }
  return AULA_STATUS_OK;
}

static int settings_temporary_is_valid(int descriptor) {
  struct stat details;
  return fchmod(descriptor, 0600) == 0 && fsync(descriptor) == 0 &&
         fstat(descriptor, &details) == 0 && S_ISREG(details.st_mode) &&
         details.st_nlink == 1 && details.st_uid == geteuid() &&
         (details.st_mode & 0777U) == 0600U;
}

static aula_status settings_write_temporary(int descriptor, aula_bytes state) {
  aula_status status = settings_write_all(descriptor, state);
  if (status != AULA_STATUS_OK) return status;
  return settings_temporary_is_valid(descriptor) ? AULA_STATUS_OK
                                                 : AULA_STATUS_IO_ERROR;
}

static aula_status settings_close_temporary(int *descriptor,
                                              aula_status status) {
  if (*descriptor >= 0 && close(*descriptor) != 0 &&
      status == AULA_STATUS_OK) {
    status = AULA_STATUS_IO_ERROR;
  }
  *descriptor = -1;
  return status;
}

#if defined(AULA_SIPD_TEST_FAULTS)
static int fail_next_settings_parent_sync;
static int fail_next_settings_rename;
void endpoint_settings_test_fail_next_rename(void) {
  fail_next_settings_rename = 1;
}
void endpoint_settings_test_fail_next_parent_sync(void) {
  fail_next_settings_parent_sync = 1;
}
#endif

static aula_status settings_commit_temporary(int parent_descriptor,
                                               const char *temporary,
                                               const char *name) {
#if defined(AULA_SIPD_TEST_FAULTS)
  if (fail_next_settings_rename != 0) {
    fail_next_settings_rename = 0;
    errno = EIO;
    return AULA_STATUS_IO_ERROR;
  }
#endif
  if (renameat(parent_descriptor, temporary, parent_descriptor, name) != 0) {
    return AULA_STATUS_IO_ERROR;
  }
  /* The rename is the transaction commit point: after it succeeds the new
   * settings are authoritative and may be observed by a restart. */
#if defined(AULA_SIPD_TEST_FAULTS)
  if (fail_next_settings_parent_sync != 0) {
    fail_next_settings_parent_sync = 0;
    errno = EIO;
    return AULA_STATUS_PERSISTENCE_UNCERTAIN;
  }
#endif
  return fsync(parent_descriptor) == 0 ? AULA_STATUS_OK
                                      : AULA_STATUS_PERSISTENCE_UNCERTAIN;
}

static aula_status settings_encode_for_endpoint(
    const aula_endpoint *endpoint, uint32_t revision, aula_zoom_profile profile,
    int media_managed, aula_mutable_bytes *encoded,
    const char **out_path) {
  if (endpoint == NULL || endpoint->view == NULL ||
      endpoint->view->runtime_settings_file == NULL ||
      endpoint->view->runtime_settings_file[0] == '\0') {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  *out_path = endpoint->view->runtime_settings_file;
  return settings_encode(revision, profile, media_managed, encoded);
}

aula_status endpoint_settings_persist(const aula_endpoint *endpoint,
                                       uint32_t revision,
                                       aula_zoom_profile profile,
                                       int media_managed) {
  uint8_t state[AULA_SETTINGS_STATE_BYTES];
  aula_mutable_bytes encoded = {state, sizeof(state), 0U};
  char parent[PATH_MAX];
  char temporary[96] = {0};
  char *name = NULL;
  int parent_descriptor = -1;
  int temporary_descriptor = -1;
  aula_status status;
  const char *path;
  status = settings_encode_for_endpoint(endpoint, revision, profile, media_managed,
                                        &encoded, &path);
  if (status == AULA_STATUS_OK) {
    status = settings_open_parent(path, parent, sizeof(parent), &name, &parent_descriptor);
  }
  if (status == AULA_STATUS_OK) {
    status = settings_open_temporary(parent_descriptor, temporary, sizeof(temporary),
                                     &temporary_descriptor);
  }
  if (status == AULA_STATUS_OK) {
    status = settings_write_temporary(temporary_descriptor,
        (aula_bytes){encoded.data, encoded.length});
  }
  status = settings_close_temporary(&temporary_descriptor, status);
  if (status == AULA_STATUS_OK) {
    status = settings_commit_temporary(parent_descriptor, temporary, name);
  }
  if (status != AULA_STATUS_OK && parent_descriptor >= 0 && temporary[0] != '\0') {
    (void)unlinkat(parent_descriptor, temporary, 0);
  }
  if (parent_descriptor >= 0) (void)close(parent_descriptor);
  (void)memset(state, 0, sizeof(state));
  return status;
}

aula_status endpoint_settings_load(aula_endpoint *endpoint) {
  uint8_t state[AULA_SETTINGS_STATE_BYTES];
  aula_mutable_bytes input = {state, sizeof(state), 0U};
  aula_control_settings_request decoded;
  const char *path;
  aula_status status;
  if (endpoint == NULL || endpoint->view == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  endpoint->settings_revision = 1U;
  endpoint->settings_media_managed = 1;
  path = endpoint->view->runtime_settings_file;
  if (path == NULL || path[0] == '\0') return AULA_STATUS_OK;
  status = aula_config_validate_runtime_settings_path(path);
  if (status == AULA_STATUS_OK && state_file_is_absent(path)) return AULA_STATUS_OK;
  if (status == AULA_STATUS_OK) status = settings_read(path, &input);
  if (status == AULA_STATUS_OK)
    status = aula_control_decode_settings((aula_bytes){input.data, input.length}, &decoded);
  if (status == AULA_STATUS_OK)
    status = endpoint_settings_validate_candidate(endpoint, decoded.profile,
                                                  decoded.media_managed);
  if (status == AULA_STATUS_OK) {
    endpoint->sip_config.profile = decoded.profile;
    if (decoded.profile != AULA_ZOOM_PROFILE_PRIVATE_LAB) {
      endpoint->sip_config.enable_tls = 1;
      endpoint->sip_config.preferred_transport = AULA_TRANSPORT_TLS;
    }
    endpoint->settings_revision = decoded.revision;
    endpoint->settings_media_managed = decoded.media_managed;
  }
  (void)memset(state, 0, sizeof(state));
  return status;
}
