#include "sdp_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void ls200_sdp_secure_zero(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

int ls200_sdp_ascii_equal(const char *left, const char *right) {
  size_t index;
  if (left == NULL || right == NULL) {
    return 0;
  }
  for (index = 0U; left[index] != '\0' && right[index] != '\0'; ++index) {
    if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) {
      return 0;
    }
  }
  return left[index] == '\0' && right[index] == '\0';
}

int ls200_sdp_parse_u32(const char *text, uint32_t *out_value) {
  uint32_t value = 0U;
  const char *cursor;
  if (text == NULL || out_value == NULL || *text == '\0' || *text == '-') {
    return 0;
  }
  for (cursor = text; *cursor != '\0'; ++cursor) {
    uint32_t digit;
    if (*cursor < '0' || *cursor > '9') return 0;
    digit = (uint32_t)(*cursor - '0');
    if (value > (UINT32_MAX - digit) / 10U) return 0;
    value = value * 10U + digit;
  }
  *out_value = value;
  return 1;
}

int ls200_sdp_parse_u16(const char *text, uint16_t *out_value) {
  uint32_t value;
  if (!ls200_sdp_parse_u32(text, &value) || value > UINT16_MAX) return 0;
  *out_value = (uint16_t)value;
  return 1;
}

int ls200_sdp_parse_payload(const char *text, uint8_t *out_value) {
  uint16_t value;
  if (!ls200_sdp_parse_u16(text, &value) || value > 127U) {
    return 0;
  }
  *out_value = (uint8_t)value;
  return 1;
}

int ls200_sdp_split_words(char *text, char **words, size_t maximum,
                                 size_t *out_count) {
  size_t count = 0U;
  char *cursor = text;
  if (text == NULL || words == NULL || out_count == NULL) {
    return 0;
  }
  while (*cursor != '\0') {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    if (count == maximum) {
      return 0;
    }
    words[count++] = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
    if (*cursor == ' ') {
      *cursor++ = '\0';
    }
  }
  *out_count = count;
  return 1;
}

int ls200_sdp_valid_text(const char *text) {
  size_t index;
  if (text == NULL || *text == '\0') {
    return 0;
  }
  for (index = 0U; text[index] != '\0'; ++index) {
    unsigned char character = (unsigned char)text[index];
    if (character < 0x20U || character > 0x7eU) {
      return 0;
    }
  }
  return 1;
}

ls200_status ls200_sdp_parse_ipv4(const char *text,
                                         unsigned int octets[4]) {
  const char *cursor = text;
  size_t index;
  if (text == NULL || octets == NULL) {
    return LS200_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < 4U; ++index) {
    unsigned long value;
    char *end;
    if (*cursor == '\0' || *cursor == '-' || !isdigit((unsigned char)*cursor)) {
      return LS200_STATUS_INVALID_DATA;
    }
    value = strtoul(cursor, &end, 10);
    if (value > 255UL || (index < 3U && *end != '.') ||
        (index == 3U && *end != '\0')) {
      return LS200_STATUS_INVALID_DATA;
    }
    octets[index] = (unsigned int)value;
    cursor = end + 1;
  }
  return LS200_STATUS_OK;
}

static int ls200_sdp_ipv4_is_unusable_local(const unsigned int octets[4]) {
  return octets[0] == 0U || octets[0] >= 224U ||
         (octets[0] == 169U && octets[1] == 254U) ||
         (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U && octets[3] == 255U);
}

static int ls200_sdp_ipv4_is_local_scope(const unsigned int octets[4]) {
  return octets[0] == 127U || octets[0] == 10U ||
         (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
         (octets[0] == 192U && octets[1] == 168U);
}

ls200_status ls200_sdp_validate_local_ipv4(const char *text) {
  unsigned int octets[4];
  ls200_status status = ls200_sdp_parse_ipv4(text, octets);
  if (status != LS200_STATUS_OK) return status;
  if (ls200_sdp_ipv4_is_unusable_local(octets)) {
    return LS200_STATUS_SECURITY_ERROR;
  }
  return ls200_sdp_ipv4_is_local_scope(octets) ? LS200_STATUS_OK : LS200_STATUS_SECURITY_ERROR;
}

ls200_status ls200_sdp_authorize_remote_ipv4(
    const ls200_sdp_session *local, const char *text, uint16_t port,
    int is_rtcp) {
  unsigned int octets[4];
  ls200_status status;
  if (local == NULL || port == 0U || local->remote_target_authorizer == NULL)
    return LS200_STATUS_SECURITY_ERROR;
  status = ls200_sdp_parse_ipv4(text, octets);
  if (status != LS200_STATUS_OK) return status;
  /* Unspecified, multicast, link-local, and broadcast destinations are never
   * valid media targets. Loopback is delegated to the mandatory target
   * authorizer so the explicit fixture/private-lab profile can operate while
   * production profiles continue to reject it through peer pinning. */
  if (octets[0] == 0U || octets[0] >= 224U ||
      (octets[0] == 169U && octets[1] == 254U) ||
      (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U && octets[3] == 255U))
    return LS200_STATUS_SECURITY_ERROR;
  return local->remote_target_authorizer(local->remote_target_context, text,
                                          port, is_rtcp != 0);
}

ls200_sdp_codec *ls200_sdp_find_codec(ls200_sdp_stored_media *media,
                                      uint8_t payload_type) {
  return (ls200_sdp_codec *)ls200_sdp_find_codec_const(media, payload_type);
}

const ls200_sdp_codec *ls200_sdp_find_codec_const(
    const ls200_sdp_stored_media *media, uint8_t payload_type) {
  size_t index;
  if (media == NULL) {
    return NULL;
  }
  for (index = 0U; index < media->value.codec_count; ++index) {
    if (media->codecs[index].payload_type == payload_type) {
      return &media->codecs[index];
    }
  }
  return NULL;
}

ls200_status ls200_sdp_add_codec(ls200_sdp_stored_media *media,
                                        ls200_sdp_codec_kind kind,
                                        uint8_t payload_type,
                                        uint32_t clock_rate,
                                        uint8_t channels) {
  size_t index;
  if (media == NULL || ls200_sdp_find_codec(media, payload_type) != NULL) {
    return LS200_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == kind && kind != LS200_SDP_CODEC_H264) {
      return LS200_STATUS_INVALID_DATA;
    }
  }
  if (media->value.codec_count >= LS200_SDP_MAX_CODECS) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  index = media->value.codec_count;
  media->codecs[index].kind = kind;
  media->codecs[index].payload_type = payload_type;
  media->codecs[index].clock_rate = clock_rate;
  media->codecs[index].channels = channels;
  media->fmtp[index][0] = '\0';
  media->codecs[index].format_parameters = media->fmtp[index];
  ++media->value.codec_count;
  return LS200_STATUS_OK;
}

static ls200_sdp_stored_media *ls200_sdp_media_for_name(
    ls200_sdp_session *session, const char *name) {
  if (ls200_sdp_ascii_equal(name, "video")) return &session->video;
  if (ls200_sdp_ascii_equal(name, "audio")) return &session->audio;
  return NULL;
}

static ls200_status ls200_sdp_initialize_media(
    ls200_sdp_stored_media *media, int is_video, uint16_t port) {
  if (media->present) return LS200_STATUS_INVALID_DATA;
  media->present = 1;
  media->is_video = is_video;
  media->value.rtp_port = port;
  media->value.rtcp_port = (uint16_t)(port + 1U);
  media->value.direction = LS200_SDP_SENDRECV;
  media->value.codecs = media->codecs;
  media->value.codec_count = 0U;
  media->value.rtcp_mux = 0;
  media->value.profile = LS200_SDP_MEDIA_PROFILE_RTP_AVP;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_add_static_audio_codec(
    ls200_sdp_stored_media *media, uint8_t payload_type) {
  if (media->is_video && (payload_type == 0U || payload_type == 8U)) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (!media->is_video && payload_type == 0U) {
    return ls200_sdp_add_codec(media, LS200_SDP_CODEC_PCMU, payload_type, 8000U, 1U);
  }
  if (!media->is_video && payload_type == 8U) {
    return ls200_sdp_add_codec(media, LS200_SDP_CODEC_PCMA, payload_type, 8000U, 1U);
  }
  return LS200_STATUS_OK;
}

static int ls200_sdp_rtpmap_matches_static_audio(
    const ls200_sdp_stored_media *media, uint8_t payload_type,
    const char *encoding, uint32_t rate, uint16_t channels) {
  if (payload_type != 0U && payload_type != 8U && payload_type != 9U) return 1;
  if (media->is_video || rate != 8000U || channels != 1U) return 0;
  if (payload_type == 0U) return ls200_sdp_ascii_equal(encoding, "PCMU");
  if (payload_type == 8U) return ls200_sdp_ascii_equal(encoding, "PCMA");
  if (payload_type == 9U) return ls200_sdp_ascii_equal(encoding, "G722");
  return 1;
}

static ls200_status ls200_sdp_parse_media_payloads(
    ls200_sdp_stored_media *media, char *const words[], size_t count) {
  size_t index;
  for (index = 3U; index < count; ++index) {
    uint8_t payload_type;
    ls200_status status;
    if (!ls200_sdp_parse_payload(words[index], &payload_type) ||
        media->declared_payload_types[payload_type] != 0U) {
      return LS200_STATUS_INVALID_DATA;
    }
    media->declared_payload_types[payload_type] = 1U;
    status = ls200_sdp_add_static_audio_codec(media, payload_type);
    if (status != LS200_STATUS_OK) return status;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_parse_media(ls200_sdp_session *session, char *value,
                                   ls200_sdp_stored_media **out_current) {
  char *words[20];
  size_t count;
  uint16_t port;
  ls200_sdp_stored_media *media;
  if (!ls200_sdp_split_words(value, words, 20U, &count) || count < 4U ||
      !ls200_sdp_parse_u16(words[1], &port) || port == 0U || (port & 1U) != 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  media = ls200_sdp_media_for_name(session, words[0]);
  if (media == NULL) return LS200_STATUS_UNSUPPORTED;
  if (ls200_sdp_initialize_media(media, ls200_sdp_ascii_equal(words[0], "video"), port) !=
      LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  if (ls200_sdp_ascii_equal(words[2], "RTP/SAVP")) {
    if (!ls200_sdp_srtp_is_available()) return LS200_STATUS_UNSUPPORTED;
    media->value.profile = LS200_SDP_MEDIA_PROFILE_RTP_SAVP;
  } else if (!ls200_sdp_ascii_equal(words[2], "RTP/AVP")) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (ls200_sdp_parse_media_payloads(media, words, count) != LS200_STATUS_OK)
    return LS200_STATUS_INVALID_DATA;
  *out_current = media;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_parse_rtpmap_fields(char *value, uint8_t *payload_type,
                                                   char **encoding, uint32_t *rate,
                                                   uint16_t *channels) {
  char *space = strchr(value, ' ');
  char *slash;
  char *channels_text = NULL;
  if (space == NULL || space == value || space[1] == '\0') return LS200_STATUS_INVALID_DATA;
  *space++ = '\0';
  if (!ls200_sdp_parse_payload(value, payload_type)) return LS200_STATUS_INVALID_DATA;
  *encoding = space;
  slash = strchr(*encoding, '/');
  if (slash == NULL || slash == *encoding || slash[1] == '\0') return LS200_STATUS_INVALID_DATA;
  *slash++ = '\0';
  channels_text = strchr(slash, '/');
  if (channels_text != NULL) *channels_text++ = '\0';
  if (!ls200_sdp_parse_u32(slash, rate) ||
      (channels_text != NULL && !ls200_sdp_parse_u16(channels_text, channels)) ||
      *channels == 0U || *channels > 2U) return LS200_STATUS_INVALID_DATA;
  return LS200_STATUS_OK;
}

static ls200_sdp_codec_kind ls200_sdp_rtpmap_codec_kind(const char *encoding,
                                                        uint32_t rate, uint16_t channels) {
  if (rate == 90000U && channels == 1U && ls200_sdp_ascii_equal(encoding, "H264"))
    return LS200_SDP_CODEC_H264;
  if (rate == 8000U && channels == 1U && ls200_sdp_ascii_equal(encoding, "PCMU"))
    return LS200_SDP_CODEC_PCMU;
  if (rate == 8000U && channels == 1U && ls200_sdp_ascii_equal(encoding, "PCMA"))
    return LS200_SDP_CODEC_PCMA;
  if (rate == 8000U && channels == 1U && ls200_sdp_ascii_equal(encoding, "telephone-event"))
    return LS200_SDP_CODEC_TELEPHONE_EVENT;
  return (ls200_sdp_codec_kind)-1;
}

static ls200_status ls200_sdp_add_rtpmap_codec(ls200_sdp_stored_media *media,
                                                uint8_t payload_type,
                                                ls200_sdp_codec_kind kind,
                                                uint32_t rate, uint16_t channels) {
  ls200_sdp_codec *existing;
  if ((media->is_video && kind != LS200_SDP_CODEC_H264) ||
      (!media->is_video && kind == LS200_SDP_CODEC_H264)) return LS200_STATUS_INVALID_DATA;
  existing = ls200_sdp_find_codec(media, payload_type);
  if (existing != NULL) {
    return existing->kind == kind && existing->clock_rate == rate &&
                   existing->channels == (uint8_t)channels ? LS200_STATUS_OK :
                                                            LS200_STATUS_INVALID_DATA;
  }
  return ls200_sdp_add_codec(media, kind, payload_type, rate, (uint8_t)channels);
}

static void ls200_sdp_log_rtpmap_failure(ls200_status status,
    const ls200_sdp_stored_media *media, uint8_t payload, uint32_t rate,
    uint16_t channels, int kind, int trailing_space, int duplicate) {
  (void)fprintf(stderr,
      "ls200-sipd: sdp-stage=rtpmap code=%d payload=%u rate=%lu channels=%u codec=%d declared=%d trailing_space=%d duplicate=%d codec_count=%u\n",
      (int)status, (unsigned)payload, (unsigned long)rate, (unsigned)channels,
      kind, media != NULL && media->declared_payload_types[payload] != 0U,
      trailing_space, duplicate, media == NULL ? 0U :
          (unsigned)media->value.codec_count);
  (void)fflush(stderr);
}

ls200_status ls200_sdp_parse_rtpmap(ls200_sdp_stored_media *media,
                                    char *value) {
  char *encoding = NULL;
  uint8_t payload_type = 0U;
  uint32_t rate = 0U;
  uint16_t channels = 1U;
  ls200_sdp_codec_kind kind = (ls200_sdp_codec_kind)-1;
  ls200_status status;
  size_t length = value == NULL ? 0U : strlen(value);
  int trailing_space = length != 0U && value[length - 1U] == ' ';
  int duplicate = 0;
  if (media == NULL || value == NULL) return LS200_STATUS_INVALID_DATA;
  status = ls200_sdp_parse_rtpmap_fields(value, &payload_type, &encoding,
                                         &rate, &channels);
  if (status == LS200_STATUS_OK) {
    kind = ls200_sdp_rtpmap_codec_kind(encoding, rate, channels);
    duplicate = media->mapped_payload_types[payload_type] != 0U;
    if (media->declared_payload_types[payload_type] == 0U)
      status = LS200_STATUS_INVALID_DATA;
    else if (duplicate ||
             !ls200_sdp_rtpmap_matches_static_audio(
                 media, payload_type, encoding, rate, channels))
      status = LS200_STATUS_INVALID_DATA;
    else if ((int)kind >= 0)
      status = ls200_sdp_add_rtpmap_codec(media, payload_type, kind, rate,
                                          channels);
    if (status == LS200_STATUS_OK)
      media->mapped_payload_types[payload_type] = 1U;
  }
  if (status != LS200_STATUS_OK)
    ls200_sdp_log_rtpmap_failure(status, media, payload_type, rate, channels,
                                 (int)kind, trailing_space, duplicate);
  return status;
}

ls200_status ls200_sdp_parse_fmtp(ls200_sdp_stored_media *media,
                                         char *value) {
  char *space = strchr(value, ' ');
  uint8_t payload_type;
  ls200_sdp_codec *codec;
  size_t index;
  if (media == NULL || space == NULL || space == value || space[1] == '\0') {
    return LS200_STATUS_INVALID_DATA;
  }
  *space++ = '\0';
  if (!ls200_sdp_parse_payload(value, &payload_type)) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (media->declared_payload_types[payload_type] == 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  codec = ls200_sdp_find_codec(media, payload_type);
  if (codec == NULL) {
    return LS200_STATUS_OK;
  }
  if (!ls200_sdp_valid_text(space) || strlen(space) >= LS200_SDP_MAX_FMTP_BYTES) {
    return LS200_STATUS_INVALID_DATA;
  }
  index = (size_t)(codec - media->codecs);
  (void)memcpy(media->fmtp[index], space, strlen(space) + 1U);
  return LS200_STATUS_OK;
}

static int ls200_sdp_h264_profile_is_valid(const char *value) {
  size_t index;
  if (strlen(value) != 6U) return 0;
  for (index = 0U; index < 6U; ++index) {
    if (!isxdigit((unsigned char)value[index])) return 0;
  }
  return 1;
}

static int ls200_sdp_h264_fmtp_field_is_valid(char *field, int *has_profile,
                                               int *has_packetization) {
  char *equals;
  while (*field == ' ') ++field;
  equals = strchr(field, '=');
  if (equals == NULL || equals == field || equals[1] == '\0') return 0;
  *equals++ = '\0';
  if (ls200_sdp_ascii_equal(field, "profile-level-id")) {
    if (!ls200_sdp_h264_profile_is_valid(equals)) return 0;
    *has_profile = 1;
  } else if (ls200_sdp_ascii_equal(field, "packetization-mode")) {
    if (strcmp(equals, "1") != 0) return 0;
    *has_packetization = 1;
  }
  return 1;
}

int ls200_sdp_h264_fmtp_is_compatible(const char *fmtp) {
  char copy[LS200_SDP_MAX_FMTP_BYTES];
  char *field;
  int has_profile = 0;
  int has_packetization = 0;
  if (fmtp == NULL || *fmtp == '\0' || strlen(fmtp) >= sizeof(copy)) {
    return 0;
  }
  (void)memcpy(copy, fmtp, strlen(fmtp) + 1U);
  field = strtok(copy, ";");
  while (field != NULL) {
    if (!ls200_sdp_h264_fmtp_field_is_valid(field, &has_profile, &has_packetization)) return 0;
    field = strtok(NULL, ";");
  }
  return has_profile && has_packetization;
}
