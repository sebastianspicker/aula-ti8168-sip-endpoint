#include "rtsp_private.h"
#include <stdlib.h>
#include <string.h>

int ls200_rtsp_path_character(unsigned char character) {
  return (character >= 'a' && character <= 'z') ||
         (character >= 'A' && character <= 'Z') ||
         (character >= '0' && character <= '9') || character == '/' ||
         character == '_' || character == '-' || character == '.';
}

static int header_is(const uint8_t *line, size_t length, const char *name) {
  size_t index;
  size_t name_length = strlen(name);
  if (length < name_length + 1U || line[name_length] != ':') return 0;
  for (index = 0U; index < name_length; ++index) {
    unsigned char actual = line[index];
    unsigned char expected = (unsigned char)name[index];
    if (actual >= 'A' && actual <= 'Z') actual = (unsigned char)(actual + ('a' - 'A'));
    if (expected >= 'A' && expected <= 'Z') expected = (unsigned char)(expected + ('a' - 'A'));
    if (actual != expected) return 0;
  }
  return 1;
}

static int parse_decimal(const uint8_t *data, size_t length, size_t maximum,
                         size_t *out_value) {
  size_t index;
  size_t value = 0U;
  if (data == NULL || length == 0U || out_value == NULL) return 0;
  for (index = 0U; index < length; ++index) {
    if (data[index] < '0' || data[index] > '9' ||
        value > (maximum - (size_t)(data[index] - '0')) / 10U) return 0;
    value = (value * 10U) + (size_t)(data[index] - '0');
  }
  *out_value = value;
  return 1;
}

static void discard_message(ls200_rtsp_stream_parser *parser) {
  if (parser->message_active == 0) return;
  if (parser->consumed < parser->length) {
    (void)memmove(parser->buffer, parser->buffer + parser->consumed,
                  parser->length - parser->consumed);
  }
  parser->length -= parser->consumed;
  parser->consumed = 0U;
  parser->message_active = 0;
}

static int find_header_end(const uint8_t *data, size_t length, size_t *out_length) {
  size_t index;
  if (length < 4U) return 0;
  for (index = 0U; index + 4U <= length; ++index) {
    if (data[index] == '\r' && data[index + 1U] == '\n' &&
        data[index + 2U] == '\r' && data[index + 3U] == '\n') {
      *out_length = index + 4U;
      return 1;
    }
  }
  return 0;
}

typedef struct rtsp_response_headers {
  size_t content_length;
  uint32_t session_timeout_seconds;
  int content_length_seen;
  int cseq_seen;
  int session_seen;
  int session_timeout_seen;
} rtsp_response_headers;

static int span_is(const uint8_t *value, size_t length, const char *expected) {
  size_t index;
  if (value == NULL || expected == NULL || strlen(expected) != length) return 0;
  for (index = 0U; index < length; ++index) {
    unsigned char actual = value[index];
    unsigned char wanted = (unsigned char)expected[index];
    if (actual >= 'A' && actual <= 'Z') actual = (unsigned char)(actual + ('a' - 'A'));
    if (wanted >= 'A' && wanted <= 'Z') wanted = (unsigned char)(wanted + ('a' - 'A'));
    if (actual != wanted) return 0;
  }
  return 1;
}

static int response_status_valid(const uint8_t *buffer, size_t header_length) {
  return header_length >= 17U && memcmp(buffer, "RTSP/1.0 ", 9U) == 0 &&
      buffer[9] >= '1' && buffer[9] <= '5' && buffer[10] >= '0' &&
      buffer[10] <= '9' && buffer[11] >= '0' && buffer[11] <= '9' &&
      buffer[12] == ' ';
}

static int response_line_printable(const uint8_t *line, size_t line_length) {
  size_t index;
  for (index = 0U; index < line_length; ++index) {
    if (line[index] < 0x20U || line[index] > 0x7eU) return 0;
  }
  return 1;
}

static ls200_status parse_response_content_length(const uint8_t *line, size_t length,
                                                  rtsp_response_headers *headers) {
  size_t start = strlen("content-length") + 1U;
  if (headers->content_length_seen != 0) return LS200_STATUS_INVALID_DATA;
  while (start < length && line[start] == ' ') start++;
  if (!parse_decimal(line + start, length - start, LS200_RTSP_MAX_BODY_BYTES,
                     &headers->content_length)) return LS200_STATUS_INVALID_DATA;
  headers->content_length_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status parse_response_cseq(const uint8_t *line, size_t length,
                                        rtsp_response_headers *headers,
                                        ls200_rtsp_message *message) {
  size_t start = strlen("cseq") + 1U;
  size_t value = 0U;
  if (headers->cseq_seen != 0) return LS200_STATUS_INVALID_DATA;
  while (start < length && line[start] == ' ') start++;
  if (!parse_decimal(line + start, length - start, (size_t)UINT32_MAX, &value)) {
    return LS200_STATUS_INVALID_DATA;
  }
  message->cseq = (uint32_t)value;
  headers->cseq_seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status parse_session_timeout_parameter(
    const uint8_t *line, size_t equals, size_t parameter_end,
    rtsp_response_headers *headers) {
  size_t value_start = equals + 1U;
  size_t value_end = parameter_end;
  size_t timeout = 0U;
  if (headers->session_timeout_seen != 0 || equals >= parameter_end ||
      line[equals] != '=') return LS200_STATUS_INVALID_DATA;
  while (value_start < value_end && line[value_start] == ' ') value_start++;
  while (value_end > value_start && line[value_end - 1U] == ' ') value_end--;
  if (!parse_decimal(line + value_start, value_end - value_start,
                     (size_t)UINT32_MAX, &timeout) || timeout == 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  headers->session_timeout_seconds = (uint32_t)timeout;
  headers->session_timeout_seen = 1;
  return LS200_STATUS_OK;
}

static size_t skip_spaces(const uint8_t *line, size_t length, size_t cursor) {
  while (cursor < length && line[cursor] == ' ') cursor++;
  return cursor;
}

static size_t session_parameter_name_end(const uint8_t *line, size_t length,
                                         size_t cursor) {
  while (cursor < length && line[cursor] != '=' && line[cursor] != ';' &&
         line[cursor] != ' ') cursor++;
  return cursor;
}

static size_t session_parameter_end(const uint8_t *line, size_t length,
                                    size_t cursor) {
  while (cursor < length && line[cursor] != ';') cursor++;
  return cursor;
}

static ls200_status parse_session_parameter(
    const uint8_t *line, size_t length, size_t *cursor,
    rtsp_response_headers *headers) {
  size_t name_start;
  size_t name_length;
  size_t parameter_end;
  size_t equals;
  *cursor = skip_spaces(line, length, *cursor);
  if (*cursor == length) return LS200_STATUS_OK;
  if (line[(*cursor)++] != ';') return LS200_STATUS_INVALID_DATA;
  *cursor = skip_spaces(line, length, *cursor);
  name_start = *cursor;
  *cursor = session_parameter_name_end(line, length, *cursor);
  name_length = *cursor - name_start;
  if (name_length == 0U) return LS200_STATUS_INVALID_DATA;
  *cursor = skip_spaces(line, length, *cursor);
  equals = *cursor;
  parameter_end = session_parameter_end(line, length, *cursor);
  *cursor = parameter_end;
  if (span_is(line + name_start, name_length, "timeout") == 0) {
    return LS200_STATUS_OK;
  }
  return parse_session_timeout_parameter(
      line, equals, parameter_end, headers);
}

static ls200_status parse_session_parameters(
    const uint8_t *line, size_t length, size_t cursor,
    rtsp_response_headers *headers) {
  while (cursor < length) {
    ls200_status status = parse_session_parameter(
        line, length, &cursor, headers);
    if (status != LS200_STATUS_OK) return status;
  }
  return LS200_STATUS_OK;
}

static ls200_status parse_response_session(const uint8_t *line, size_t length,
                                           rtsp_response_headers *headers,
                                           ls200_rtsp_message *message) {
  size_t start = strlen("session") + 1U;
  size_t value_length = 0U;
  if (headers->session_seen != 0) return LS200_STATUS_INVALID_DATA;
  while (start < length && line[start] == ' ') start++;
  while (start + value_length < length && line[start + value_length] != ';' &&
         line[start + value_length] != ' ') value_length++;
  if (value_length == 0U || value_length >= sizeof(message->session_id)) {
    return LS200_STATUS_INVALID_DATA;
  }
  (void)memcpy(message->session_id, line + start, value_length);
  message->session_id[value_length] = '\0';
  headers->session_seen = 1;
  return parse_session_parameters(
      line, length, start + value_length, headers);
}

static ls200_status parse_response_header_line(const uint8_t *line, size_t length,
                                               rtsp_response_headers *headers,
                                               ls200_rtsp_message *message) {
  if (!response_line_printable(line, length)) return LS200_STATUS_INVALID_DATA;
  if (header_is(line, length, "content-length")) {
    return parse_response_content_length(line, length, headers);
  }
  if (header_is(line, length, "cseq")) return parse_response_cseq(line, length, headers, message);
  if (header_is(line, length, "session"))
    return parse_response_session(line, length, headers, message);
  return LS200_STATUS_OK;
}

static ls200_status parse_response(ls200_rtsp_stream_parser *parser,
                                   ls200_rtsp_message *out_message) {
  size_t header_length;
  size_t line_start;
  rtsp_response_headers headers = {0};
  if (!find_header_end(parser->buffer, parser->length, &header_length)) {
    return parser->length > LS200_RTSP_MAX_HEADER_BYTES ? LS200_STATUS_LIMIT_EXCEEDED :
        LS200_STATUS_AGAIN;
  }
  if (header_length > LS200_RTSP_MAX_HEADER_BYTES ||
      !response_status_valid(parser->buffer, header_length)) return LS200_STATUS_INVALID_DATA;
  out_message->kind = LS200_RTSP_MESSAGE_RESPONSE;
  out_message->status_code = (uint16_t)(((uint16_t)(parser->buffer[9] - '0') * 100U) +
                                        ((uint16_t)(parser->buffer[10] - '0') * 10U) +
                                        (uint16_t)(parser->buffer[11] - '0'));
  out_message->cseq = 0U;
  out_message->channel = 0U;
  out_message->session_id[0] = '\0';
  line_start = 0U;
  while (line_start + 2U < header_length) {
    size_t line_end = line_start;
    while (line_end + 1U < header_length &&
           !(parser->buffer[line_end] == '\r' && parser->buffer[line_end + 1U] == '\n')) line_end++;
    if (line_end + 1U >= header_length) return LS200_STATUS_INVALID_DATA;
    if (line_start != 0U) {
      const uint8_t *line = parser->buffer + line_start;
      size_t line_length = line_end - line_start;
      if (line_length == 0U) break;
      if (parse_response_header_line(line, line_length, &headers, out_message) != LS200_STATUS_OK) {
        return LS200_STATUS_INVALID_DATA;
      }
    }
    line_start = line_end + 2U;
  }
  if (headers.cseq_seen == 0) return LS200_STATUS_INVALID_DATA;
  if (headers.content_length > parser->length - header_length) return LS200_STATUS_AGAIN;
  out_message->body.data = parser->buffer + header_length;
  out_message->body.length = headers.content_length;
  parser->session_timeout_seconds = headers.session_timeout_seconds;
  parser->session_timeout_present = headers.session_timeout_seen;
  parser->consumed = header_length + headers.content_length;
  parser->message_active = 1;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_stream_parser_create(ls200_rtsp_stream_parser **out_parser) {
  ls200_rtsp_stream_parser *parser;
  if (out_parser == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  parser = (ls200_rtsp_stream_parser *)calloc(1U, sizeof(*parser));
  if (parser == NULL) return LS200_STATUS_INTERNAL_ERROR;
  *out_parser = parser;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_stream_parser_push(ls200_rtsp_stream_parser *parser,
                                           ls200_bytes input,
                                           ls200_rtsp_message *out_message) {
  size_t payload_length;
  if (parser == NULL || out_message == NULL || (input.data == NULL && input.length != 0U)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  discard_message(parser);
  if (input.length > sizeof(parser->buffer) - parser->length) return LS200_STATUS_LIMIT_EXCEEDED;
  if (input.length != 0U) {
    (void)memcpy(parser->buffer + parser->length, input.data, input.length);
    parser->length += input.length;
  }
  if (parser->length == 0U) return LS200_STATUS_AGAIN;
  if (parser->buffer[0] != '$') return parse_response(parser, out_message);
  if (parser->length < 4U) return LS200_STATUS_AGAIN;
  payload_length = ((size_t)parser->buffer[2] << 8U) | (size_t)parser->buffer[3];
  if (parser->buffer[1] > 3U || payload_length == 0U ||
      payload_length > LS200_SIPD_MAX_RTP_PACKET_BYTES) return LS200_STATUS_INVALID_DATA;
  if (payload_length > parser->length - 4U) return LS200_STATUS_AGAIN;
  out_message->kind = LS200_RTSP_MESSAGE_INTERLEAVED_RTP;
  out_message->status_code = 0U;
  out_message->cseq = 0U;
  out_message->channel = parser->buffer[1];
  out_message->session_id[0] = '\0';
  out_message->body.data = parser->buffer + 4U;
  out_message->body.length = payload_length;
  parser->consumed = payload_length + 4U;
  parser->message_active = 1;
  return LS200_STATUS_OK;
}

void ls200_rtsp_stream_parser_destroy(ls200_rtsp_stream_parser *parser) { free(parser); }

int ls200_rtsp_stream_parser_session_timeout(
    const ls200_rtsp_stream_parser *parser, uint32_t *out_seconds) {
  if (parser == NULL || out_seconds == NULL ||
      parser->session_timeout_present == 0) return 0;
  *out_seconds = parser->session_timeout_seconds;
  return 1;
}

static int rtsp_safe_path(const char *path) {
  const char *cursor;
  if (path == NULL || path[0] != '/') return 0;
  for (cursor = path; *cursor != '\0'; ++cursor) {
    if (!ls200_rtsp_path_character((unsigned char)*cursor)) return 0;
  }
  return strstr(path, "//") == NULL && strstr(path, "/../") == NULL &&
      strcmp(path, "/..") != 0 && strcmp(path, "/") != 0;
}

static int parse_ipv4_octet(const char **cursor, uint8_t *out_octet) {
  unsigned int value = 0U;
  size_t digits = 0U;
  if (**cursor == '0' && (*cursor)[1] >= '0' && (*cursor)[1] <= '9') return 0;
  while (**cursor >= '0' && **cursor <= '9') {
    value = value * 10U + (unsigned int)(**cursor - '0');
    if (++digits > 3U || value > 255U) return 0;
    ++*cursor;
  }
  if (digits == 0U) return 0;
  *out_octet = (uint8_t)value;
  return 1;
}

static int parse_canonical_ipv4(const char *value, uint8_t out_address[4]) {
  const char *cursor = value;
  size_t index;
  if (value == NULL || out_address == NULL || value[0] == '\0') return 0;
  for (index = 0U; index < 4U; ++index) {
    if (!parse_ipv4_octet(&cursor, &out_address[index])) return 0;
    if (index < 3U && *cursor++ != '.') return 0;
  }
  return *cursor == '\0';
}

int ls200_rtsp_ipv4_is_loopback(const uint8_t address[4]) {
  return address != NULL && address[0] == 127U;
}

static int rtsp_authorized_address(const uint8_t address[4]) {
  static const uint8_t unspecified[4] = {0U, 0U, 0U, 0U};
  static const uint8_t broadcast[4] = {255U, 255U, 255U, 255U};
  if (address == NULL || memcmp(address, unspecified, sizeof(unspecified)) == 0 ||
      memcmp(address, broadcast, sizeof(broadcast)) == 0) return 0;
  if (ls200_rtsp_ipv4_is_loopback(address)) return 1;
  if (address[0] == 10U) return 1;
  if (address[0] == 172U && address[1] >= 16U && address[1] <= 31U) return 1;
  if (address[0] == 192U && address[1] == 168U) return 1;
  return address[0] == 169U && address[1] == 254U;
}

int ls200_rtsp_ipv4_literal_matches(const char *value, const uint8_t address[4]) {
  uint8_t parsed[4];
  return parse_canonical_ipv4(value, parsed) && address != NULL &&
      memcmp(parsed, address, sizeof(parsed)) == 0;
}

static int parse_rtsp_uri_host(const char **cursor, uint8_t out_address[4]) {
  const char *start = *cursor;
  size_t length;
  char host[16];
  while (**cursor != '\0' && **cursor != ':' && **cursor != '/') ++*cursor;
  length = (size_t)(*cursor - start);
  if (length == 0U || length >= sizeof(host)) return 0;
  (void)memcpy(host, start, length);
  host[length] = '\0';
  return parse_canonical_ipv4(host, out_address);
}

static int parse_rtsp_uri_port(const char **cursor, size_t *out_port) {
  const char *start;
  if (**cursor != ':') return 1;
  start = ++*cursor;
  while (**cursor >= '0' && **cursor <= '9') ++*cursor;
  return parse_decimal((const uint8_t *)start, (size_t)(*cursor - start),
                       65535U, out_port) && *out_port != 0U;
}

static int parse_authorized_target(const char *value, uint8_t out_address[4]) {
  return parse_canonical_ipv4(value, out_address) &&
      rtsp_authorized_address(out_address);
}

ls200_status ls200_rtsp_parse_authorized_uri(const char *uri,
                                             const char *authorized_ipv4,
                                             ls200_rtsp_uri *out_uri) {
  static const char prefix[] = "rtsp://";
  uint8_t authorized_address[4];
  uint8_t uri_address[4];
  const char *cursor;
  size_t length;
  size_t port = 554U;
  if (out_uri == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (uri == NULL || authorized_ipv4 == NULL) return LS200_STATUS_CONFIGURATION_ERROR;
  if (!parse_authorized_target(authorized_ipv4, authorized_address) ||
      strncmp(uri, prefix, sizeof(prefix) - 1U) != 0) {
    return LS200_STATUS_CONFIGURATION_ERROR;
  }
  cursor = uri + sizeof(prefix) - 1U;
  if (!parse_rtsp_uri_host(&cursor, uri_address) ||
      memcmp(uri_address, authorized_address, sizeof(uri_address)) != 0) {
    return LS200_STATUS_CONFIGURATION_ERROR;
  }
  if (!parse_rtsp_uri_port(&cursor, &port) || !rtsp_safe_path(cursor)) {
    return LS200_STATUS_CONFIGURATION_ERROR;
  }
  length = strlen(uri);
  if (length >= sizeof(out_uri->text)) return LS200_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(out_uri->text, uri, length + 1U);
  (void)memcpy(out_uri->address, authorized_address, sizeof(out_uri->address));
  out_uri->port = (uint16_t)port;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_parse_local_uri(const char *uri, ls200_rtsp_uri *out_uri) {
  return ls200_rtsp_parse_authorized_uri(uri, "127.0.0.1", out_uri);
}

ls200_status ls200_rtsp_local_uri_validate(const char *uri) {
  ls200_rtsp_uri parsed;
  return ls200_rtsp_parse_local_uri(uri, &parsed);
}

ls200_status ls200_rtsp_authorized_uri_validate(const char *uri,
                                                const char *authorized_ipv4) {
  ls200_rtsp_uri parsed;
  return ls200_rtsp_parse_authorized_uri(uri, authorized_ipv4, &parsed);
}
