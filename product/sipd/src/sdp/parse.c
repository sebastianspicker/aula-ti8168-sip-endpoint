#include "sdp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ls200_sdp_parse_context {
  ls200_sdp_session *session;
  ls200_sdp_stored_media *current;
  ls200_sdp_direction session_direction;
  int seen_version;
  int seen_origin;
  int seen_name;
  int seen_time;
  int seen_connection;
  int has_session_direction;
} ls200_sdp_parse_context;

static ls200_status ls200_sdp_parse_required_field(const char *value, int *seen,
                                                    int exact_version) {
  if (*seen || !ls200_sdp_valid_text(value) ||
      (exact_version && strcmp(value, "0") != 0)) return LS200_STATUS_INVALID_DATA;
  *seen = 1;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_parse_connection(ls200_sdp_parse_context *context,
                                                char *value) {
  char *words[4];
  size_t count;
  unsigned int octets[4];
  if (context->current != NULL || context->seen_connection ||
      !ls200_sdp_split_words(value, words, 4U, &count) || count != 3U ||
      !ls200_sdp_ascii_equal(words[0], "IN") || !ls200_sdp_ascii_equal(words[1], "IP4") ||
      strlen(words[2]) >= sizeof(context->session->connection_address) ||
      !ls200_sdp_valid_text(words[2]) ||
      ls200_sdp_parse_ipv4(words[2], octets) != LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  (void)memcpy(context->session->connection_address, words[2], strlen(words[2]) + 1U);
  context->seen_connection = 1;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_parse_rtcp_address(ls200_sdp_stored_media *media,
                                                  char *value) {
  char *words[4];
  size_t count;
  unsigned int octets[4];
  if (!ls200_sdp_split_words(value, words, 4U, &count) || count != 3U ||
      !ls200_sdp_ascii_equal(words[0], "IN") || !ls200_sdp_ascii_equal(words[1], "IP4") ||
      strlen(words[2]) >= sizeof(media->rtcp_address) ||
      ls200_sdp_parse_ipv4(words[2], octets) != LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  (void)memcpy(media->rtcp_address, words[2], strlen(words[2]) + 1U);
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_parse_rtcp(ls200_sdp_stored_media *media, char *value) {
  char *space;
  uint16_t port;
  if (media == NULL) return LS200_STATUS_INVALID_DATA;
  space = strchr(value, ' ');
  if (space != NULL) *space++ = '\0';
  if (!ls200_sdp_parse_u16(value, &port) || port == 0U ||
      (space != NULL && !ls200_sdp_valid_text(space))) return LS200_STATUS_INVALID_DATA;
  media->value.rtcp_port = port;
  return space == NULL ? LS200_STATUS_OK : ls200_sdp_parse_rtcp_address(media, space);
}

static int ls200_sdp_is_direction_attribute(const char *attribute) {
  return ls200_sdp_ascii_equal(attribute, "sendrecv") ||
         ls200_sdp_ascii_equal(attribute, "sendonly") ||
         ls200_sdp_ascii_equal(attribute, "recvonly") ||
         ls200_sdp_ascii_equal(attribute, "inactive");
}

static ls200_status ls200_sdp_parse_attribute(ls200_sdp_parse_context *context,
                                              char *attribute) {
  if (strncmp(attribute, "rtcp-fb:", 8U) == 0)
    return ls200_sdp_parse_feedback(context->current, attribute + 8);
  if (ls200_sdp_ascii_equal(attribute, "rtcp-mux")) {
    if (context->current == NULL) return LS200_STATUS_INVALID_DATA;
    context->current->value.rtcp_mux = 1;
    context->current->value.rtcp_port = context->current->value.rtp_port;
    return LS200_STATUS_OK;
  }
  if (strncmp(attribute, "rtcp:", 5U) == 0)
    return ls200_sdp_parse_rtcp(context->current, attribute + 5);
  if (strncmp(attribute, "rtpmap:", 7U) == 0)
    return ls200_sdp_parse_rtpmap(context->current, attribute + 7);
  if (strncmp(attribute, "fmtp:", 5U) == 0)
    return ls200_sdp_parse_fmtp(context->current, attribute + 5);
  if (strncmp(attribute, "crypto:", 7U) == 0)
    return ls200_sdp_parse_crypto(context->current, attribute + 7);
  if (ls200_sdp_is_direction_attribute(attribute)) {
    return ls200_sdp_set_direction(context->current, attribute, &context->session_direction,
                                   &context->has_session_direction);
  }
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_parse_line(ls200_sdp_parse_context *context, char *line) {
  switch (line[0]) {
    case 'v': return ls200_sdp_parse_required_field(line + 2, &context->seen_version, 1);
    case 'o': return ls200_sdp_parse_required_field(line + 2, &context->seen_origin, 0);
    case 's': return ls200_sdp_parse_required_field(line + 2, &context->seen_name, 0);
    case 't': return ls200_sdp_parse_required_field(line + 2, &context->seen_time, 0);
    case 'c': return ls200_sdp_parse_connection(context, line + 2);
    case 'm': return ls200_sdp_parse_media(context->session, line + 2, &context->current);
    case 'a': return ls200_sdp_parse_attribute(context, line + 2);
    default: return LS200_STATUS_OK;
  }
}

static ls200_status ls200_sdp_next_line(char *copy, size_t length, size_t *offset,
                                        char **out_line) {
  size_t start = *offset;
  size_t line_length;
  while (*offset < length && copy[*offset] != '\n') {
    if (copy[*offset] == '\r' && (*offset + 1U >= length || copy[*offset + 1U] != '\n'))
      return LS200_STATUS_INVALID_DATA;
    ++*offset;
  }
  line_length = *offset - start;
  if (*offset < length) ++*offset;
  if (line_length > 0U && copy[start + line_length - 1U] == '\r') --line_length;
  if (line_length < 2U || line_length > LS200_SDP_MAX_LINE_BYTES || copy[start + 1U] != '=')
    return LS200_STATUS_INVALID_DATA;
  copy[start + line_length] = '\0';
  if (!ls200_sdp_valid_text(&copy[start])) return LS200_STATUS_INVALID_DATA;
  *out_line = &copy[start];
  return LS200_STATUS_OK;
}

static void ls200_sdp_apply_session_direction(ls200_sdp_parse_context *context) {
  if (!context->has_session_direction) return;
  if (context->session->video.present && !context->session->video.direction_explicit)
    context->session->video.value.direction = context->session_direction;
  if (context->session->audio.present && !context->session->audio.direction_explicit)
    context->session->audio.value.direction = context->session_direction;
}

static ls200_status ls200_sdp_validate_parsed_media(const ls200_sdp_parse_context *context,
                                                     int strict_offer, int video) {
  return strict_offer ? ls200_sdp_validate_media(video ? &context->session->video :
                                                   &context->session->audio, video) :
                        ls200_sdp_validate_answer_media(video ? &context->session->video :
                                                          &context->session->audio, video);
}

static ls200_status ls200_sdp_validate_parsed_session(const ls200_sdp_parse_context *context,
                                                       int strict_offer) {
  ls200_status status;
  if (!context->seen_version || !context->seen_origin || !context->seen_name ||
      !context->seen_time || !context->seen_connection) return LS200_STATUS_INVALID_DATA;
  status = ls200_sdp_validate_parsed_media(context, strict_offer, 1);
  return status == LS200_STATUS_OK ?
             ls200_sdp_validate_parsed_media(context, strict_offer, 0) : status;
}

static int ls200_sdp_diagnostic_field(const char *line) {
  if (line[0] == 'c') return 1;
  if (line[0] == 'm') return 2;
  if (line[0] != 'a') return 0;
  if (strncmp(line + 2, "crypto:", 7U) == 0) return 3;
  if (strncmp(line + 2, "rtpmap:", 7U) == 0) return 4;
  if (strncmp(line + 2, "fmtp:", 5U) == 0) return 5;
  if (strncmp(line + 2, "rtcp", 4U) == 0) return 6;
  return 7;
}

/* Field identifiers are fixed categories; never print input SDP or keys. */
static void ls200_sdp_log_parse_failure(const char *stage, ls200_status status,
    unsigned line_number, int field, const ls200_sdp_parse_context *context) {
  (void)fprintf(stderr,
      "ls200-sipd: sdp-stage=%s code=%d line=%u field=%d media_scope=%d session_connection=%d video_present=%d audio_present=%d\n",
      stage, (int)status, line_number, field, context->current != NULL,
      context->seen_connection, context->session->video.present,
      context->session->audio.present);
  (void)fflush(stderr);
}

static ls200_status ls200_sdp_validate_and_log(
    const ls200_sdp_parse_context *context, int strict_offer,
    unsigned line_number) {
  ls200_status status = ls200_sdp_validate_parsed_session(context, strict_offer);
  if (status == LS200_STATUS_OK) return status;
  ls200_sdp_log_parse_failure("validate-session", status, line_number, 0,
                              context);
  ls200_sdp_log_codec_diagnostics(context->session, strict_offer, 1);
  return status;
}

static ls200_status ls200_sdp_parse_session(ls200_bytes input, int strict_offer,
                                            ls200_sdp_session **out_session) {
  ls200_sdp_parse_context context;
  char *copy;
  size_t offset = 0U;
  unsigned line_number = 0U;
  int field = 0;
  ls200_status status = LS200_STATUS_OK;
  if (out_session == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_session = NULL;
  if (input.data == NULL || input.length == 0U || input.length > LS200_SIPD_MAX_SDP_BYTES)
    return LS200_STATUS_INVALID_ARGUMENT;
  copy = (char *)malloc(input.length + 1U);
  (void)memset(&context, 0, sizeof(context));
  context.session = (ls200_sdp_session *)calloc(1U, sizeof(*context.session));
  if (copy == NULL || context.session == NULL) {
    free(copy);
    free(context.session);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  (void)memcpy(copy, input.data, input.length);
  copy[input.length] = '\0';
  while (offset < input.length && status == LS200_STATUS_OK) {
    char *line;
    ++line_number;
    status = ls200_sdp_next_line(copy, input.length, &offset, &line);
    if (status == LS200_STATUS_OK) {
      field = ls200_sdp_diagnostic_field(line);
      status = ls200_sdp_parse_line(&context, line);
    }
    if (status != LS200_STATUS_OK)
      ls200_sdp_log_parse_failure("parse-line", status, line_number, field,
                                   &context);
  }
  if (status == LS200_STATUS_OK) ls200_sdp_apply_session_direction(&context);
  if (status == LS200_STATUS_OK)
    status = ls200_sdp_validate_and_log(&context, strict_offer, line_number);
  ls200_sdp_log_crypto_diagnostics(input, status);
  ls200_sdp_secure_zero(copy, input.length + 1U);
  free(copy);
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(context.session);
    return status;
  }
  *out_session = context.session;
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_parse_offer(ls200_bytes input, ls200_sdp_session **out_session) {
  return ls200_sdp_parse_session(input, 1, out_session);
}

ls200_status ls200_sdp_parse_answer(ls200_bytes input, ls200_sdp_session **out_session) {
  return ls200_sdp_parse_session(input, 0, out_session);
}
