#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "config_private.h"
#include <ctype.h>
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

void aula_config_diagnose(aula_config_diagnostic *diagnostic, aula_status status,
                          unsigned int line, const char *setting, const char *reason) {
  size_t index;
  if (diagnostic == NULL) return;
  diagnostic->status = status;
  diagnostic->line = line;
  diagnostic->reason = reason;
  (void)snprintf(diagnostic->setting, sizeof(diagnostic->setting), "%s",
                 setting == NULL ? "" : setting);
  /* The setting may echo file text; keep it to printable ASCII. */
  for (index = 0U; diagnostic->setting[index] != '\0'; ++index) {
    unsigned char character = (unsigned char)diagnostic->setting[index];
    if (character < 0x20U || character > 0x7eU) diagnostic->setting[index] = '?';
  }
}

static aula_status update_section(config_load_state *state, char *text, const char **reason) {
  size_t length = strlen(text);
  *reason = "malformed section header";
  if (length < 3U || text[length - 1U] != ']') return AULA_STATUS_INVALID_DATA;
  text[length - 1U] = '\0';
  if (strlen(text + 1) >= sizeof(state->section) || strchr(text + 1, ' ') != NULL) return AULA_STATUS_INVALID_DATA;
  (void)snprintf(state->section, sizeof(state->section), "%s", text + 1);
  *reason = "unknown section";
  return strcmp(state->section, "runtime") == 0 || strcmp(state->section, "sip") == 0 || strcmp(state->section, "media") == 0 || strcmp(state->section, "control") == 0 || strcmp(state->section, "limits") == 0 ? AULA_STATUS_OK : AULA_STATUS_CONFIGURATION_ERROR;
}

static aula_status apply_config_assignment(aula_config *config, config_load_state *state, char *text,
                                           const char **reason) {
  char *equals = strchr(text, '=');
  char *key;
  char *value;
  config_key id;
  *reason = state->section[0] == '\0' ? "setting appears before any [section] header"
                                      : "expected a single key = value";
  if (state->section[0] == '\0' || equals == NULL || strchr(equals + 1, '=') != NULL) return AULA_STATUS_INVALID_DATA;
  *equals = '\0'; key = aula_config_trim(text); value = aula_config_trim(equals + 1);
  *reason = "missing key before '='";
  if (key[0] == '\0') return AULA_STATUS_INVALID_DATA;
  *reason = "value is too long or contains control characters or '..' segments";
  if (strlen(value) >= AULA_CONFIG_VALUE_BYTES || aula_config_has_control_or_parent_segment(value)) return AULA_STATUS_INVALID_DATA;
  *reason = "unknown or duplicate key";
  if (aula_config_key_for(state->section, key, &id) != AULA_STATUS_OK || state->seen[id] != 0U) return AULA_STATUS_CONFIGURATION_ERROR;
  state->seen[id] = 1U;
  *reason = "invalid value";
  return aula_config_apply_key(config, id, value);
}

/* Length of the leading run of identifier characters: section and key names
 * use only these, so nothing after them (a value) is ever echoed. */
static size_t identifier_length(const char *text) {
  size_t length = 0U;
  while (text[length] == '_' || (text[length] >= 'a' && text[length] <= 'z') ||
         (text[length] >= 'A' && text[length] <= 'Z') ||
         (text[length] >= '0' && text[length] <= '9')) ++length;
  return length;
}

/* Names the section or key a failing line refers to, without its value. A
 * line without '=' may be a mistyped value, so only its section is named. */
static void describe_line(const config_load_state *state, const char *text,
                          char *setting, size_t capacity) {
  size_t length;
  if (text[0] == '[') {
    length = identifier_length(text + 1);
    if (text[1 + length] == ']')
      (void)snprintf(setting, capacity, "[%.*s]", (int)length, text + 1);
    else
      (void)snprintf(setting, capacity, "section header");
    return;
  }
  length = strchr(text, '=') != NULL ? identifier_length(text) : 0U;
  if (state->section[0] == '\0' && length == 0U)
    (void)snprintf(setting, capacity, "line");
  else if (state->section[0] == '\0')
    (void)snprintf(setting, capacity, "%.*s", (int)length, text);
  else if (length == 0U)
    (void)snprintf(setting, capacity, "[%.40s]", state->section);
  else
    (void)snprintf(setting, capacity, "%.20s.%.*s", state->section, (int)length, text);
}

static aula_status parse_config_stream(FILE *input, aula_config *config,
                                       aula_config_diagnostic *diagnostic) {
  char line[AULA_CONFIG_LINE_BYTES];
  char setting[sizeof(diagnostic->setting)];
  config_load_state state;
  aula_status status = AULA_STATUS_OK;
  unsigned int line_number = 0U;
  (void)memset(&state, 0, sizeof(state));
  while (fgets(line, (int)sizeof(line), input) != NULL) {
    char *text;
    const char *reason = NULL;
    size_t length = strlen(line);
    ++line_number;
    state.bytes_read += length;
    if (state.bytes_read > AULA_CONFIG_FILE_BYTES || (length == sizeof(line) - 1U && line[length - 1U] != '\n')) {
      status = AULA_STATUS_LIMIT_EXCEEDED;
      aula_config_diagnose(diagnostic, status, line_number, "file",
                           "line or file exceeds the size limit");
      break;
    }
    text = aula_config_trim(line);
    if (text[0] == '\0' || text[0] == '#') continue;
    describe_line(&state, text, setting, sizeof(setting));
    status = text[0] == '[' ? update_section(&state, text, &reason) : apply_config_assignment(config, &state, text, &reason);
    if (status != AULA_STATUS_OK) {
      aula_config_diagnose(diagnostic, status, line_number, setting, reason);
      break;
    }
  }
  if (ferror(input) != 0 && status == AULA_STATUS_OK) {
    status = AULA_STATUS_IO_ERROR;
    aula_config_diagnose(diagnostic, status, 0U, "file", "read failed");
  }
  return status;
}

aula_status aula_config_load_descriptor(int descriptor,
                                           int require_unlinked_snapshot,
                                           aula_config **out_config,
                                           aula_config_diagnostic *diagnostic) {
  FILE *input;
  aula_config *config;
  aula_status status;
  if (descriptor < 0 || out_config == NULL || *out_config != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_config = NULL;
  status = validate_config_descriptor(descriptor, require_unlinked_snapshot);
  if (status != AULA_STATUS_OK) {
    aula_config_diagnose(diagnostic, status, 0U, "file",
        status == AULA_STATUS_SECURITY_ERROR ? "descriptor must carry the write, grow, shrink and seal seals"
        : status == AULA_STATUS_UNSUPPORTED ? "sealed descriptors are supported only on Linux"
        : status == AULA_STATUS_IO_ERROR ? "cannot read"
        : "must be a regular file owned by the daemon user, with one link, "
          "not group or world writable");
    return status;
  }
  input = fdopen(descriptor, "r");
  if (input == NULL) {
    (void)close(descriptor);
    aula_config_diagnose(diagnostic, AULA_STATUS_IO_ERROR, 0U, "file", "cannot read");
    return AULA_STATUS_IO_ERROR;
  }
  config = (aula_config *)calloc(1U, sizeof(*config));
  if (config == NULL) {
    (void)fclose(input);
    aula_config_diagnose(diagnostic, AULA_STATUS_INTERNAL_ERROR, 0U, "file", "out of memory");
    return AULA_STATUS_INTERNAL_ERROR;
  }
  aula_config_defaults(config);
  status = parse_config_stream(input, config, diagnostic);
  (void)fclose(input);
  if (status == AULA_STATUS_OK) {
    status = aula_config_capture_inherited_fixture_fds(config, require_unlinked_snapshot);
    if (status != AULA_STATUS_OK)
      aula_config_diagnose(diagnostic, status, 0U, "media.fixture descriptors",
                           "inherited fixture descriptors are invalid");
  }
  if (status == AULA_STATUS_OK) status = aula_config_validate(config, diagnostic);
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

static const char *open_failure_reason(aula_status status) {
  if (status == AULA_STATUS_CONFIGURATION_ERROR)
    return "path must be absolute and must not contain '..' segments";
  if (status == AULA_STATUS_SECURITY_ERROR) return "must not be a symbolic link";
  if (status == AULA_STATUS_IO_ERROR) return "cannot open";
  if (status == AULA_STATUS_LIMIT_EXCEEDED) return "path is too long";
  return "parent directory must exist, be owned by root or the daemon user and not be "
         "group or world writable; the file must be a regular file owned by the daemon user";
}

aula_status aula_config_load_file_diagnosed(const char *path,
                                              aula_config **out_config,
                                              aula_config_diagnostic *diagnostic) {
  int descriptor;
  aula_status status;
  aula_config_diagnose(diagnostic, AULA_STATUS_OK, 0U, "", NULL);
  if (path == NULL || out_config == NULL || *out_config != NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_config_open_owned_regular_file(path, 0, &descriptor);
  if (status != AULA_STATUS_OK) {
    aula_config_diagnose(diagnostic, status, 0U, "file", open_failure_reason(status));
    return status;
  }
  return aula_config_load_descriptor(descriptor, 0, out_config, diagnostic);
}

aula_status aula_config_load_file(const char *path, aula_config **out_config) {
  return aula_config_load_file_diagnosed(path, out_config, NULL);
}

aula_status aula_config_load_fd_diagnosed(int descriptor,
                                            aula_config **out_config,
                                            aula_config_diagnostic *diagnostic) {
  int duplicate;
  aula_config_diagnose(diagnostic, AULA_STATUS_OK, 0U, "", NULL);
  if (descriptor < 0 || out_config == NULL || *out_config != NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  duplicate = dup(descriptor);
  if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
    if (duplicate >= 0) (void)close(duplicate);
    aula_config_diagnose(diagnostic, AULA_STATUS_IO_ERROR, 0U, "file", "cannot duplicate descriptor");
    return AULA_STATUS_IO_ERROR;
  }
  return aula_config_load_descriptor(duplicate, 1, out_config, diagnostic);
}

aula_status aula_config_load_fd(int descriptor, aula_config **out_config) {
  return aula_config_load_fd_diagnosed(descriptor, out_config, NULL);
}
