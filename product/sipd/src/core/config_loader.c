#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "config_private.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS (F_LINUX_SPECIFIC_BASE + 10)
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif
#define LS200_REQUIRED_CONFIG_SEALS (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)
#endif

typedef struct config_load_state {
  char section[64];
  unsigned char seen[CONFIG_KEY_COUNT];
  size_t bytes_read;
} config_load_state;

static ls200_status validate_config_descriptor(int descriptor, int require_unlinked_snapshot) {
  struct stat details;
#if defined(__linux__)
  if (require_unlinked_snapshot != 0 &&
      fcntl(descriptor, F_GET_SEALS) != LS200_REQUIRED_CONFIG_SEALS) {
    (void)close(descriptor);
    return LS200_STATUS_SECURITY_ERROR;
  }
#else
  if (require_unlinked_snapshot != 0) {
    (void)close(descriptor);
    return LS200_STATUS_UNSUPPORTED;
  }
#endif
  if (fstat(descriptor, &details) != 0 || !S_ISREG(details.st_mode) ||
      details.st_uid != geteuid() ||
      (require_unlinked_snapshot != 0 &&
       details.st_nlink != 0) ||
      (require_unlinked_snapshot == 0 &&
       (details.st_nlink != 1 || (details.st_mode & 0022U) != 0U))) {
    (void)close(descriptor);
    return LS200_STATUS_PERMISSION_DENIED;
  }
  if (lseek(descriptor, 0, SEEK_SET) < 0) {
    (void)close(descriptor);
    return LS200_STATUS_IO_ERROR;
  }
  return LS200_STATUS_OK;
}

static ls200_status update_section(config_load_state *state, char *text) {
  size_t length = strlen(text);
  if (length < 3U || text[length - 1U] != ']') return LS200_STATUS_INVALID_DATA;
  text[length - 1U] = '\0';
  if (strlen(text + 1) >= sizeof(state->section) || strchr(text + 1, ' ') != NULL) return LS200_STATUS_INVALID_DATA;
  (void)snprintf(state->section, sizeof(state->section), "%s", text + 1);
  return strcmp(state->section, "runtime") == 0 || strcmp(state->section, "sip") == 0 || strcmp(state->section, "media") == 0 || strcmp(state->section, "control") == 0 || strcmp(state->section, "limits") == 0 ? LS200_STATUS_OK : LS200_STATUS_CONFIGURATION_ERROR;
}

static ls200_status apply_config_assignment(ls200_config *config, config_load_state *state, char *text) {
  char *equals = strchr(text, '=');
  char *key;
  char *value;
  config_key id;
  if (state->section[0] == '\0' || equals == NULL || strchr(equals + 1, '=') != NULL) return LS200_STATUS_INVALID_DATA;
  *equals = '\0'; key = ls200_config_trim(text); value = ls200_config_trim(equals + 1);
  if (key[0] == '\0' || strlen(value) >= LS200_CONFIG_VALUE_BYTES || ls200_config_has_control_or_parent_segment(value)) return LS200_STATUS_INVALID_DATA;
  if (ls200_config_key_for(state->section, key, &id) != LS200_STATUS_OK || state->seen[id] != 0U) return LS200_STATUS_CONFIGURATION_ERROR;
  state->seen[id] = 1U;
  return ls200_config_apply_key(config, id, value);
}

static ls200_status parse_config_stream(FILE *input, ls200_config *config) {
  char line[LS200_CONFIG_LINE_BYTES];
  config_load_state state;
  ls200_status status = LS200_STATUS_OK;
  (void)memset(&state, 0, sizeof(state));
  while (fgets(line, (int)sizeof(line), input) != NULL) {
    char *text;
    size_t length = strlen(line);
    state.bytes_read += length;
    if (state.bytes_read > LS200_CONFIG_FILE_BYTES || (length == sizeof(line) - 1U && line[length - 1U] != '\n')) { status = LS200_STATUS_LIMIT_EXCEEDED; break; }
    text = ls200_config_trim(line);
    if (text[0] == '\0' || text[0] == '#') continue;
    status = text[0] == '[' ? update_section(&state, text) : apply_config_assignment(config, &state, text);
    if (status != LS200_STATUS_OK) break;
  }
  return ferror(input) != 0 && status == LS200_STATUS_OK ? LS200_STATUS_IO_ERROR : status;
}

ls200_status ls200_config_load_descriptor(int descriptor,
                                           int require_unlinked_snapshot,
                                           ls200_config **out_config) {
  FILE *input;
  ls200_config *config;
  ls200_status status;
  if (descriptor < 0 || out_config == NULL || *out_config != NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_config = NULL;
  status = validate_config_descriptor(descriptor, require_unlinked_snapshot);
  if (status != LS200_STATUS_OK) return status;
  input = fdopen(descriptor, "r");
  if (input == NULL) { (void)close(descriptor); return LS200_STATUS_IO_ERROR; }
  config = (ls200_config *)calloc(1U, sizeof(*config));
  if (config == NULL) { (void)fclose(input); return LS200_STATUS_INTERNAL_ERROR; }
  ls200_config_defaults(config);
  status = parse_config_stream(input, config);
  (void)fclose(input);
  if (status == LS200_STATUS_OK)
    status = ls200_config_capture_inherited_fixture_fds(config, require_unlinked_snapshot);
  if (status == LS200_STATUS_OK) status = ls200_config_validate(config);
  if (status != LS200_STATUS_OK) {
    ls200_config_close_fixture_fds(config);
    (void)memset(config, 0, sizeof(*config));
    free(config);
    return status;
  }
  ls200_config_bind_view(config);
  *out_config = config;
  return LS200_STATUS_OK;
}

ls200_status ls200_config_load_file(const char *path, ls200_config **out_config) {
  int descriptor;
  ls200_status status;
  if (path == NULL || out_config == NULL || *out_config != NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  status = ls200_config_open_owned_regular_file(path, 0, &descriptor);
  if (status != LS200_STATUS_OK) return status;
  return ls200_config_load_descriptor(descriptor, 0, out_config);
}

ls200_status ls200_config_load_fd(int descriptor, ls200_config **out_config) {
  int duplicate;
  if (descriptor < 0 || out_config == NULL || *out_config != NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  duplicate = dup(descriptor);
  if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
    if (duplicate >= 0) (void)close(duplicate);
    return LS200_STATUS_IO_ERROR;
  }
  return ls200_config_load_descriptor(duplicate, 1, out_config);
}
