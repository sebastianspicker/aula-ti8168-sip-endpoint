#include "aula_sipd/sip.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define AULA_SIP_MAX_URI_BYTES 512U

static int text_is_safe(const char *text) {
  size_t index;
  if (text == NULL || text[0] == '\0' ||
      strlen(text) >= AULA_SIP_MAX_URI_BYTES)
    return 0;
  for (index = 0U; text[index] != '\0'; ++index) {
    unsigned char character = (unsigned char)text[index];
    if (character <= 0x20U || character >= 0x7fU) return 0;
  }
  return 1;
}

static int is_user_character(unsigned char character) {
  return isalnum(character) || character == '-' || character == '_' ||
         character == '.' || character == '~' || character == '+';
}

static unsigned int hex_value(unsigned char character) {
  return isdigit(character) ? (unsigned int)(character - '0')
                            : (unsigned int)(tolower(character) - 'a' + 10);
}

static int validate_user(const char *begin, const char *end) {
  const char *cursor;
  for (cursor = begin; cursor < end; ++cursor) {
    unsigned char character = (unsigned char)*cursor;
    unsigned char decoded;
    if (is_user_character(character)) continue;
    if (character != '%' || end - cursor < 3 ||
        !isxdigit((unsigned char)cursor[1]) ||
        !isxdigit((unsigned char)cursor[2]))
      return 0;
    decoded = (unsigned char)((hex_value((unsigned char)cursor[1]) << 4U) |
                              hex_value((unsigned char)cursor[2]));
    if (!is_user_character(decoded)) return 0;
    cursor += 2;
  }
  return 1;
}

static int validate_host_label(const char *label, const char *end) {
  const char *cursor;
  size_t length = (size_t)(end - label);
  if (length == 0U || length > 63U ||
      !isalnum((unsigned char)label[0]) ||
      !isalnum((unsigned char)label[length - 1U]))
    return 0;
  for (cursor = label; cursor < end; ++cursor) {
    if (!isalnum((unsigned char)*cursor) && *cursor != '-') return 0;
  }
  return 1;
}

static int validate_host(const char *begin, const char *end) {
  const char *label = begin;
  const char *cursor;
  if (begin == end || *begin == '[') return 0;
  for (cursor = begin; cursor <= end; ++cursor) {
    if (cursor != end && *cursor != '.') continue;
    if (!validate_host_label(label, cursor)) return 0;
    label = cursor + 1;
  }
  return 1;
}

static int parse_port(const char **cursor_pointer, unsigned long *out_port) {
  const char *cursor = *cursor_pointer;
  unsigned long port = 0UL;
  if (*cursor != ':') {
    *out_port = 5060UL;
    return 1;
  }
  ++cursor;
  if (!isdigit((unsigned char)*cursor)) return 0;
  while (isdigit((unsigned char)*cursor)) {
    port = port * 10UL + (unsigned long)(*cursor - '0');
    if (port > 65535UL) return 0;
    ++cursor;
  }
  if (port == 0UL) return 0;
  *cursor_pointer = cursor;
  *out_port = port;
  return 1;
}

static int validate_parameters(const char *cursor) {
  int transport_seen = 0;
  while (*cursor != '\0') {
    const char *value;
    size_t length;
    if (*cursor != ';') return 0;
    value = ++cursor;
    while (*cursor != '\0' && *cursor != ';') ++cursor;
    length = (size_t)(cursor - value);
    if (transport_seen || length != 13U ||
        (strncmp(value, "transport=udp", length) != 0 &&
         strncmp(value, "transport=tcp", length) != 0))
      return 0;
    transport_seen = 1;
  }
  return 1;
}

static int validate_uri(const char *uri) {
  const char *at;
  const char *host_end;
  const char *cursor;
  unsigned long port;
  if (!text_is_safe(uri) || strncmp(uri, "sip:", 4U) != 0) return 0;
  at = strchr(uri + 4U, '@');
  if (at == NULL || at == uri + 4U || at[1] == '\0' ||
      strchr(at + 1U, '@') != NULL ||
      memchr(uri + 4U, ':', (size_t)(at - (uri + 4U))) != NULL)
    return 0;
  if (!validate_user(uri + 4U, at)) return 0;
  host_end = at + 1U;
  while (*host_end != '\0' && *host_end != ':' && *host_end != ';') ++host_end;
  if (!validate_host(at + 1U, host_end)) return 0;
  cursor = host_end;
  if (!parse_port(&cursor, &port)) return 0;
  return validate_parameters(cursor);
}

static void classify_user(const char *begin, const char *end,
                          aula_sip_dial_target *target) {
  const char *cursor;
  uint32_t components = 1U;
  int numeric = 1;
  int compound = 0;
  for (cursor = begin; cursor < end; ++cursor) {
    if (isdigit((unsigned char)*cursor)) continue;
    if ((*cursor == '.' || *cursor == '+') && cursor != begin &&
        cursor + 1U < end && isdigit((unsigned char)cursor[-1]) &&
        isdigit((unsigned char)cursor[1])) {
      compound = 1;
      ++components;
    } else {
      numeric = 0;
    }
  }
  target->kind = numeric ? (compound ? AULA_SIP_DIAL_NUMERIC_COMPOUND
                                     : AULA_SIP_DIAL_NUMERIC)
                         : AULA_SIP_DIAL_GENERIC;
  target->numeric_component_count = numeric ? components : 0U;
}

aula_status aula_sip_parse_dial_target(const char *uri,
                                         aula_sip_dial_target *out_target) {
  const char *at;
  const char *host_end;
  const char *cursor;
  unsigned long port;
  if (out_target == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_target, 0, sizeof(*out_target));
  if (!validate_uri(uri)) return AULA_STATUS_INVALID_DATA;
  at = strchr(uri + 4U, '@');
  classify_user(uri + 4U, at, out_target);
  host_end = at + 1U;
  while (*host_end != '\0' && *host_end != ':' && *host_end != ';') ++host_end;
  cursor = host_end;
  out_target->has_explicit_port = *cursor == ':';
  (void)parse_port(&cursor, &port);
  out_target->transport = strstr(cursor, "transport=tcp") == NULL
                              ? AULA_TRANSPORT_UDP
                              : AULA_TRANSPORT_TCP;
  out_target->port = (uint16_t)port;
  return AULA_STATUS_OK;
}

aula_status aula_sip_format_redacted_dial_target(
    const aula_sip_dial_target *target, aula_mutable_bytes *output) {
  const char *kind;
  const char *transport;
  int needed;
  if (target == NULL || output == NULL || output->data == NULL ||
      output->capacity == 0U || target->kind < AULA_SIP_DIAL_GENERIC ||
      target->kind > AULA_SIP_DIAL_NUMERIC_COMPOUND ||
      (target->transport != AULA_TRANSPORT_UDP &&
       target->transport != AULA_TRANSPORT_TCP))
    return AULA_STATUS_INVALID_ARGUMENT;
  kind = target->kind == AULA_SIP_DIAL_NUMERIC
             ? "numeric"
             : (target->kind == AULA_SIP_DIAL_NUMERIC_COMPOUND
                    ? "numeric_compound"
                    : "generic");
  transport = target->transport == AULA_TRANSPORT_TCP ? "tcp" : "udp";
  needed = snprintf((char *)output->data, output->capacity,
                    "sip_target kind=%s transport=%s identity=redacted",
                    kind, transport);
  if (needed < 0 || (size_t)needed >= output->capacity) {
    output->length = 0U;
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  output->length = (size_t)needed;
  return AULA_STATUS_OK;
}
