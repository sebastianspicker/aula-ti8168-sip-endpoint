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
#define AULA_REQUIRED_CONFIG_SEALS (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)
#endif

typedef struct config_load_state {
  char section[64];
  unsigned char seen[CONFIG_KEY_COUNT];
  size_t bytes_read;
} config_load_state;

static aula_status validate_config_descriptor(int descriptor, int require_unlinked_snapshot) {
  struct stat details;
#if defined(__linux__)
  if (require_unlinked_snapshot != 0 &&
      fcntl(descriptor, F_GET_SEALS) != AULA_REQUIRED_CONFIG_SEALS) {
    (void)close(descriptor);
    return AULA_STATUS_SECURITY_ERROR;
  }
#else
  if (require_unlinked_snapshot != 0) {
    (void)close(descriptor);
    return AULA_STATUS_UNSUPPORTED;
  }
#endif
  if (fstat(descriptor, &details) != 0 || !S_ISREG(details.st_mode) ||
      details.st_uid != geteuid() ||
      (require_unlinked_snapshot != 0 &&
       details.st_nlink != 0) ||
      (require_unlinked_snapshot == 0 &&
       (details.st_nlink != 1 || (details.st_mode & 0022U) != 0U))) {
    (void)close(descriptor);
    return AULA_STATUS_PERMISSION_DENIED;
  }
  if (lseek(descriptor, 0, SEEK_SET) < 0) {
    (void)close(descriptor);
    return AULA_STATUS_IO_ERROR;
  }
  return AULA_STATUS_OK;
}

static aula_status update_section(config_load_state *state, char *text) {
  size_t length = strlen(text);
  if (length < 3U || text[length - 1U] != ']') return AULA_STATUS_INVALID_DATA;
  text[length - 1U] = '\0';
  if (strlen(text + 1) >= sizeof(state->section) || strchr(text + 1, ' ') != NULL) return AULA_STATUS_INVALID_DATA;
  (void)snprintf(state->section, sizeof(state->section), "%s", text + 1);
  return strcmp(state->section, "runtime") == 0 || strcmp(state->section, "sip") == 0 || strcmp(state->section, "media") == 0 || strcmp(state->section, "control") == 0 || strcmp(state->section, "limits") == 0 ? AULA_STATUS_OK : AULA_STATUS_CONFIGURATION_ERROR;
}

static aula_status apply_config_assignment(aula_config *config, config_load_state *state, char *text) {
  char *equals = strchr(text, '=');
  char *key;
  char *value;
  config_key id;
  if (state->section[0] == '\0' || equals == NULL || strchr(equals + 1, '=') != NULL) return AULA_STATUS_INVALID_DATA;
  *equals = '\0'; key = aula_config_trim(text); value = aula_config_trim(equals + 1);
  if (key[0] == '\0' || strlen(value) >= AULA_CONFIG_VALUE_BYTES || aula_config_has_control_or_parent_segment(value)) return AULA_STATUS_INVALID_DATA;
  if (aula_config_key_for(state->section, key, &id) != AULA_STATUS_OK || state->seen[id] != 0U) return AULA_STATUS_CONFIGURATION_ERROR;
  state->seen[id] = 1U;
  return aula_config_apply_key(config, id, value);
}

static aula_status parse_config_stream(FILE *input, aula_config *config) {
  char line[AULA_CONFIG_LINE_BYTES];
  config_load_state state;
  aula_status status = AULA_STATUS_OK;
  (void)memset(&state, 0, sizeof(state));
  while (fgets(line, (int)sizeof(line), input) != NULL) {
    char *text;
    size_t length = strlen(line);
    state.bytes_read += length;
    if (state.bytes_read > AULA_CONFIG_FILE_BYTES || (length == sizeof(line) - 1U && line[length - 1U] != '\n')) { status = AULA_STATUS_LIMIT_EXCEEDED; break; }
    text = aula_config_trim(line);
    if (text[0] == '\0' || text[0] == '#') continue;
    status = text[0] == '[' ? update_section(&state, text) : apply_config_assignment(config, &state, text);
    if (status != AULA_STATUS_OK) break;
  }
  return ferror(input) != 0 && status == AULA_STATUS_OK ? AULA_STATUS_IO_ERROR : status;
}

aula_status aula_config_load_descriptor(int descriptor,
                                           int require_unlinked_snapshot,
                                           aula_config **out_config) {
  FILE *input;
  aula_config *config;
  aula_status status;
  if (descriptor < 0 || out_config == NULL || *out_config != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_config = NULL;
  status = validate_config_descriptor(descriptor, require_unlinked_snapshot);
  if (status != AULA_STATUS_OK) return status;
  input = fdopen(descriptor, "r");
  if (input == NULL) { (void)close(descriptor); return AULA_STATUS_IO_ERROR; }
  config = (aula_config *)calloc(1U, sizeof(*config));
  if (config == NULL) { (void)fclose(input); return AULA_STATUS_INTERNAL_ERROR; }
  aula_config_defaults(config);
  status = parse_config_stream(input, config);
  (void)fclose(input);
  if (status == AULA_STATUS_OK)
    status = aula_config_capture_inherited_fixture_fds(config, require_unlinked_snapshot);
  if (status == AULA_STATUS_OK) status = aula_config_validate(config);
  if (status != AULA_STATUS_OK) {
    aula_config_close_fixture_fds(config);
    (void)memset(config, 0, sizeof(*config));
    free(config);
    return status;
  }
  aula_config_bind_view(config);
  *out_config = config;
  return AULA_STATUS_OK;
}

aula_status aula_config_load_file(const char *path, aula_config **out_config) {
  int descriptor;
  aula_status status;
  if (path == NULL || out_config == NULL || *out_config != NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_config_open_owned_regular_file(path, 0, &descriptor);
  if (status != AULA_STATUS_OK) return status;
  return aula_config_load_descriptor(descriptor, 0, out_config);
}

aula_status aula_config_load_fd(int descriptor, aula_config **out_config) {
  int duplicate;
  if (descriptor < 0 || out_config == NULL || *out_config != NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  duplicate = dup(descriptor);
  if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
    if (duplicate >= 0) (void)close(duplicate);
    return AULA_STATUS_IO_ERROR;
  }
  return aula_config_load_descriptor(duplicate, 1, out_config);
}
