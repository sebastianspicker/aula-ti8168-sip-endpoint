#include "sdp_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void aula_sdp_secure_zero(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

int aula_sdp_ascii_equal(const char *left, const char *right) {
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

int aula_sdp_parse_u32(const char *text, uint32_t *out_value) {
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

int aula_sdp_parse_u16(const char *text, uint16_t *out_value) {
  uint32_t value;
  if (!aula_sdp_parse_u32(text, &value) || value > UINT16_MAX) return 0;
  *out_value = (uint16_t)value;
  return 1;
}

int aula_sdp_parse_payload(const char *text, uint8_t *out_value) {
  uint16_t value;
  if (!aula_sdp_parse_u16(text, &value) || value > 127U) {
    return 0;
  }
  *out_value = (uint8_t)value;
  return 1;
}

int aula_sdp_split_words(char *text, char **words, size_t maximum,
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

int aula_sdp_valid_text(const char *text) {
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

aula_status aula_sdp_parse_ipv4(const char *text,
                                         unsigned int octets[4]) {
  const char *cursor = text;
  size_t index;
  if (text == NULL || octets == NULL) {
    return AULA_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < 4U; ++index) {
    unsigned long value;
    char *end;
    if (*cursor == '\0' || *cursor == '-' || !isdigit((unsigned char)*cursor)) {
      return AULA_STATUS_INVALID_DATA;
    }
    value = strtoul(cursor, &end, 10);
    if (value > 255UL || (index < 3U && *end != '.') ||
        (index == 3U && *end != '\0')) {
      return AULA_STATUS_INVALID_DATA;
    }
    octets[index] = (unsigned int)value;
    cursor = end + 1;
  }
  return AULA_STATUS_OK;
}

static int aula_sdp_ipv4_is_unusable_local(const unsigned int octets[4]) {
  return octets[0] == 0U || octets[0] >= 224U ||
         (octets[0] == 169U && octets[1] == 254U) ||
         (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U && octets[3] == 255U);
}

static int aula_sdp_ipv4_is_local_scope(const unsigned int octets[4]) {
  return octets[0] == 127U || octets[0] == 10U ||
         (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
         (octets[0] == 192U && octets[1] == 168U);
}

aula_status aula_sdp_validate_local_ipv4(const char *text) {
  unsigned int octets[4];
  aula_status status = aula_sdp_parse_ipv4(text, octets);
  if (status != AULA_STATUS_OK) return status;
  if (aula_sdp_ipv4_is_unusable_local(octets)) {
    return AULA_STATUS_SECURITY_ERROR;
  }
  return aula_sdp_ipv4_is_local_scope(octets) ? AULA_STATUS_OK : AULA_STATUS_SECURITY_ERROR;
}

aula_status aula_sdp_authorize_remote_ipv4(
    const aula_sdp_session *local, const char *text, uint16_t port,
    int is_rtcp) {
  unsigned int octets[4];
  aula_status status;
  if (local == NULL || port == 0U || local->remote_target_authorizer == NULL)
    return AULA_STATUS_SECURITY_ERROR;
  status = aula_sdp_parse_ipv4(text, octets);
  if (status != AULA_STATUS_OK) return status;
  /* Unspecified, multicast, link-local, and broadcast destinations are never
   * valid media targets. Loopback is delegated to the mandatory target
   * authorizer so the explicit fixture/private-lab profile can operate while
   * production profiles continue to reject it through peer pinning. */
  if (octets[0] == 0U || octets[0] >= 224U ||
      (octets[0] == 169U && octets[1] == 254U) ||
      (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U && octets[3] == 255U))
    return AULA_STATUS_SECURITY_ERROR;
  return local->remote_target_authorizer(local->remote_target_context, text,
                                          port, is_rtcp != 0);
}

aula_sdp_codec *aula_sdp_find_codec(aula_sdp_stored_media *media,
                                      uint8_t payload_type) {
  return (aula_sdp_codec *)aula_sdp_find_codec_const(media, payload_type);
}

const aula_sdp_codec *aula_sdp_find_codec_const(
    const aula_sdp_stored_media *media, uint8_t payload_type) {
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

aula_status aula_sdp_add_codec(aula_sdp_stored_media *media,
                                        aula_sdp_codec_kind kind,
                                        uint8_t payload_type,
                                        uint32_t clock_rate,
                                        uint8_t channels) {
  size_t index;
  if (media == NULL || aula_sdp_find_codec(media, payload_type) != NULL) {
    return AULA_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < media->value.codec_count; ++index) {
    if (media->codecs[index].kind == kind && kind != AULA_SDP_CODEC_H264) {
      return AULA_STATUS_INVALID_DATA;
    }
  }
  if (media->value.codec_count >= AULA_SDP_MAX_CODECS) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  index = media->value.codec_count;
  media->codecs[index].kind = kind;
  media->codecs[index].payload_type = payload_type;
  media->codecs[index].clock_rate = clock_rate;
  media->codecs[index].channels = channels;
  media->fmtp[index][0] = '\0';
  media->codecs[index].format_parameters = media->fmtp[index];
  ++media->value.codec_count;
  return AULA_STATUS_OK;
}

static aula_sdp_stored_media *aula_sdp_media_for_name(
    aula_sdp_session *session, const char *name) {
  if (aula_sdp_ascii_equal(name, "video")) return &session->video;
  if (aula_sdp_ascii_equal(name, "audio")) return &session->audio;
  return NULL;
}

static aula_status aula_sdp_initialize_media(
    aula_sdp_stored_media *media, int is_video, uint16_t port) {
  if (media->present) return AULA_STATUS_INVALID_DATA;
  media->present = 1;
  media->is_video = is_video;
  media->value.rtp_port = port;
  media->value.rtcp_port = (uint16_t)(port + 1U);
  media->value.direction = AULA_SDP_SENDRECV;
  media->value.codecs = media->codecs;
  media->value.codec_count = 0U;
  media->value.rtcp_mux = 0;
  media->value.profile = AULA_SDP_MEDIA_PROFILE_RTP_AVP;
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_add_static_audio_codec(
    aula_sdp_stored_media *media, uint8_t payload_type) {
  if (media->is_video && (payload_type == 0U || payload_type == 8U)) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (!media->is_video && payload_type == 0U) {
    return aula_sdp_add_codec(media, AULA_SDP_CODEC_PCMU, payload_type, 8000U, 1U);
  }
  if (!media->is_video && payload_type == 8U) {
    return aula_sdp_add_codec(media, AULA_SDP_CODEC_PCMA, payload_type, 8000U, 1U);
  }
  return AULA_STATUS_OK;
}

static int aula_sdp_rtpmap_matches_static_audio(
    const aula_sdp_stored_media *media, uint8_t payload_type,
    const char *encoding, uint32_t rate, uint16_t channels) {
  if (payload_type != 0U && payload_type != 8U && payload_type != 9U) return 1;
  if (media->is_video || rate != 8000U || channels != 1U) return 0;
  if (payload_type == 0U) return aula_sdp_ascii_equal(encoding, "PCMU");
  if (payload_type == 8U) return aula_sdp_ascii_equal(encoding, "PCMA");
  if (payload_type == 9U) return aula_sdp_ascii_equal(encoding, "G722");
  return 1;
}

static aula_status aula_sdp_parse_media_payloads(
    aula_sdp_stored_media *media, char *const words[], size_t count) {
  size_t index;
  for (index = 3U; index < count; ++index) {
    uint8_t payload_type;
    aula_status status;
    if (!aula_sdp_parse_payload(words[index], &payload_type) ||
        media->declared_payload_types[payload_type] != 0U) {
      return AULA_STATUS_INVALID_DATA;
    }
    media->declared_payload_types[payload_type] = 1U;
    status = aula_sdp_add_static_audio_codec(media, payload_type);
    if (status != AULA_STATUS_OK) return status;
  }
  return AULA_STATUS_OK;
}

aula_status aula_sdp_parse_media(aula_sdp_session *session, char *value,
                                   aula_sdp_stored_media **out_current) {
  char *words[20];
  size_t count;
  uint16_t port;
  aula_sdp_stored_media *media;
  if (!aula_sdp_split_words(value, words, 20U, &count) || count < 4U ||
      !aula_sdp_parse_u16(words[1], &port) || port == 0U || (port & 1U) != 0U) {
    return AULA_STATUS_INVALID_DATA;
  }
  media = aula_sdp_media_for_name(session, words[0]);
  if (media == NULL) return AULA_STATUS_UNSUPPORTED;
  if (aula_sdp_initialize_media(media, aula_sdp_ascii_equal(words[0], "video"), port) !=
      AULA_STATUS_OK) return AULA_STATUS_INVALID_DATA;
  if (aula_sdp_ascii_equal(words[2], "RTP/SAVP")) {
    if (!aula_sdp_srtp_is_available()) return AULA_STATUS_UNSUPPORTED;
    media->value.profile = AULA_SDP_MEDIA_PROFILE_RTP_SAVP;
  } else if (!aula_sdp_ascii_equal(words[2], "RTP/AVP")) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (aula_sdp_parse_media_payloads(media, words, count) != AULA_STATUS_OK)
    return AULA_STATUS_INVALID_DATA;
  *out_current = media;
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_parse_rtpmap_fields(char *value, uint8_t *payload_type,
                                                   char **encoding, uint32_t *rate,
                                                   uint16_t *channels) {
  char *space = strchr(value, ' ');
  char *slash;
  char *channels_text = NULL;
  if (space == NULL || space == value || space[1] == '\0') return AULA_STATUS_INVALID_DATA;
  *space++ = '\0';
  if (!aula_sdp_parse_payload(value, payload_type)) return AULA_STATUS_INVALID_DATA;
  *encoding = space;
  slash = strchr(*encoding, '/');
  if (slash == NULL || slash == *encoding || slash[1] == '\0') return AULA_STATUS_INVALID_DATA;
  *slash++ = '\0';
  channels_text = strchr(slash, '/');
  if (channels_text != NULL) *channels_text++ = '\0';
  if (!aula_sdp_parse_u32(slash, rate) ||
      (channels_text != NULL && !aula_sdp_parse_u16(channels_text, channels)) ||
      *channels == 0U || *channels > 2U) return AULA_STATUS_INVALID_DATA;
  return AULA_STATUS_OK;
}

static aula_sdp_codec_kind aula_sdp_rtpmap_codec_kind(const char *encoding,
                                                        uint32_t rate, uint16_t channels) {
  if (rate == 90000U && channels == 1U && aula_sdp_ascii_equal(encoding, "H264"))
    return AULA_SDP_CODEC_H264;
  if (rate == 8000U && channels == 1U && aula_sdp_ascii_equal(encoding, "PCMU"))
    return AULA_SDP_CODEC_PCMU;
  if (rate == 8000U && channels == 1U && aula_sdp_ascii_equal(encoding, "PCMA"))
    return AULA_SDP_CODEC_PCMA;
  if (rate == 8000U && channels == 1U && aula_sdp_ascii_equal(encoding, "telephone-event"))
    return AULA_SDP_CODEC_TELEPHONE_EVENT;
  return (aula_sdp_codec_kind)-1;
}

static aula_status aula_sdp_add_rtpmap_codec(aula_sdp_stored_media *media,
                                                uint8_t payload_type,
                                                aula_sdp_codec_kind kind,
                                                uint32_t rate, uint16_t channels) {
  aula_sdp_codec *existing;
  if ((media->is_video && kind != AULA_SDP_CODEC_H264) ||
      (!media->is_video && kind == AULA_SDP_CODEC_H264)) return AULA_STATUS_INVALID_DATA;
  existing = aula_sdp_find_codec(media, payload_type);
  if (existing != NULL) {
    return existing->kind == kind && existing->clock_rate == rate &&
                   existing->channels == (uint8_t)channels ? AULA_STATUS_OK :
                                                            AULA_STATUS_INVALID_DATA;
  }
  return aula_sdp_add_codec(media, kind, payload_type, rate, (uint8_t)channels);
}

static void aula_sdp_log_rtpmap_failure(aula_status status,
    const aula_sdp_stored_media *media, uint8_t payload, uint32_t rate,
    uint16_t channels, int kind, int trailing_space, int duplicate) {
  (void)fprintf(stderr,
      "aula-sipd: sdp-stage=rtpmap code=%d payload=%u rate=%lu channels=%u codec=%d declared=%d trailing_space=%d duplicate=%d codec_count=%u\n",
      (int)status, (unsigned)payload, (unsigned long)rate, (unsigned)channels,
      kind, media != NULL && media->declared_payload_types[payload] != 0U,
      trailing_space, duplicate, media == NULL ? 0U :
          (unsigned)media->value.codec_count);
  (void)fflush(stderr);
}

aula_status aula_sdp_parse_rtpmap(aula_sdp_stored_media *media,
                                    char *value) {
  char *encoding = NULL;
  uint8_t payload_type = 0U;
  uint32_t rate = 0U;
  uint16_t channels = 1U;
  aula_sdp_codec_kind kind = (aula_sdp_codec_kind)-1;
  aula_status status;
  size_t length = value == NULL ? 0U : strlen(value);
  int trailing_space = length != 0U && value[length - 1U] == ' ';
  int duplicate = 0;
  if (media == NULL || value == NULL) return AULA_STATUS_INVALID_DATA;
  status = aula_sdp_parse_rtpmap_fields(value, &payload_type, &encoding,
                                         &rate, &channels);
  if (status == AULA_STATUS_OK) {
    kind = aula_sdp_rtpmap_codec_kind(encoding, rate, channels);
    duplicate = media->mapped_payload_types[payload_type] != 0U;
    if (media->declared_payload_types[payload_type] == 0U)
      status = AULA_STATUS_INVALID_DATA;
    else if (duplicate ||
             !aula_sdp_rtpmap_matches_static_audio(
                 media, payload_type, encoding, rate, channels))
      status = AULA_STATUS_INVALID_DATA;
    else if ((int)kind >= 0)
      status = aula_sdp_add_rtpmap_codec(media, payload_type, kind, rate,
                                          channels);
    if (status == AULA_STATUS_OK)
      media->mapped_payload_types[payload_type] = 1U;
  }
  if (status != AULA_STATUS_OK)
    aula_sdp_log_rtpmap_failure(status, media, payload_type, rate, channels,
                                 (int)kind, trailing_space, duplicate);
  return status;
}

aula_status aula_sdp_parse_fmtp(aula_sdp_stored_media *media,
                                         char *value) {
  char *space = strchr(value, ' ');
  uint8_t payload_type;
  aula_sdp_codec *codec;
  size_t index;
  if (media == NULL || space == NULL || space == value || space[1] == '\0') {
    return AULA_STATUS_INVALID_DATA;
  }
  *space++ = '\0';
  if (!aula_sdp_parse_payload(value, &payload_type)) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (media->declared_payload_types[payload_type] == 0U) {
    return AULA_STATUS_INVALID_DATA;
  }
  codec = aula_sdp_find_codec(media, payload_type);
  if (codec == NULL) {
    return AULA_STATUS_OK;
  }
  if (!aula_sdp_valid_text(space) || strlen(space) >= AULA_SDP_MAX_FMTP_BYTES) {
    return AULA_STATUS_INVALID_DATA;
  }
  index = (size_t)(codec - media->codecs);
  (void)memcpy(media->fmtp[index], space, strlen(space) + 1U);
  return AULA_STATUS_OK;
}

static int aula_sdp_h264_profile_is_valid(const char *value) {
  size_t index;
  if (strlen(value) != 6U) return 0;
  for (index = 0U; index < 6U; ++index) {
    if (!isxdigit((unsigned char)value[index])) return 0;
  }
  return 1;
}

static int aula_sdp_h264_fmtp_field_is_valid(char *field, int *has_profile,
                                               int *has_packetization) {
  char *equals;
  while (*field == ' ') ++field;
  equals = strchr(field, '=');
  if (equals == NULL || equals == field || equals[1] == '\0') return 0;
  *equals++ = '\0';
  if (aula_sdp_ascii_equal(field, "profile-level-id")) {
    if (!aula_sdp_h264_profile_is_valid(equals)) return 0;
    *has_profile = 1;
  } else if (aula_sdp_ascii_equal(field, "packetization-mode")) {
    if (strcmp(equals, "1") != 0) return 0;
    *has_packetization = 1;
  }
  return 1;
}

int aula_sdp_h264_fmtp_is_compatible(const char *fmtp) {
  char copy[AULA_SDP_MAX_FMTP_BYTES];
  char *field;
  int has_profile = 0;
  int has_packetization = 0;
  if (fmtp == NULL || *fmtp == '\0' || strlen(fmtp) >= sizeof(copy)) {
    return 0;
  }
  (void)memcpy(copy, fmtp, strlen(fmtp) + 1U);
  field = strtok(copy, ";");
  while (field != NULL) {
    if (!aula_sdp_h264_fmtp_field_is_valid(field, &has_profile, &has_packetization)) return 0;
    field = strtok(NULL, ";");
  }
  return has_profile && has_packetization;
}
