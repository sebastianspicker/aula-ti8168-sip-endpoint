#include "sdp_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *aula_sdp_direction_name(aula_sdp_direction direction) {
  switch (direction) {
    case AULA_SDP_SENDRECV: return "sendrecv";
    case AULA_SDP_SENDONLY: return "sendonly";
    case AULA_SDP_RECVONLY: return "recvonly";
    case AULA_SDP_INACTIVE: return "inactive";
    default: return NULL;
  }
}

static const char *aula_sdp_codec_name(aula_sdp_codec_kind kind) {
  switch (kind) {
    case AULA_SDP_CODEC_H264: return "H264";
    case AULA_SDP_CODEC_PCMU: return "PCMU";
    case AULA_SDP_CODEC_PCMA: return "PCMA";
    case AULA_SDP_CODEC_TELEPHONE_EVENT: return "telephone-event";
    default: return NULL;
  }
}

static int aula_sdp_codec_order(aula_sdp_codec_kind kind) {
  switch (kind) {
    case AULA_SDP_CODEC_H264: return 0;
    case AULA_SDP_CODEC_PCMU: return 1;
    case AULA_SDP_CODEC_PCMA: return 2;
    case AULA_SDP_CODEC_TELEPHONE_EVENT: return 3;
    default: return -1;
  }
}

static aula_status aula_sdp_append(aula_mutable_bytes *output, const char *text) {
  size_t length = strlen(text);
  if (output->length > output->capacity || length > output->capacity - output->length) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  (void)memcpy(output->data + output->length, text, length);
  output->length += length;
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_append_media_description(const aula_sdp_stored_media *media,
                                                        const char *name,
                                                        aula_mutable_bytes *output) {
  size_t index;
  int rank;
  char line[AULA_SDP_MAX_LINE_BYTES];
  const char *profile = media->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP ? "RTP/AVP" :
                        media->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_SAVP ? "RTP/SAVP" :
                                                                                     NULL;
  int written;
  if (profile == NULL) return AULA_STATUS_INVALID_DATA;
  written = snprintf(line, sizeof(line), "m=%s %u %s", name,
                     (unsigned int)media->value.rtp_port, profile);
  if (written < 0 || (size_t)written >= sizeof(line)) return AULA_STATUS_INTERNAL_ERROR;
  for (rank = 0; rank < 4; ++rank) {
    for (index = 0U; index < media->value.codec_count; ++index) {
      int extension;
      if (aula_sdp_codec_order(media->codecs[index].kind) != rank) continue;
      extension = snprintf(line + (size_t)written, sizeof(line) - (size_t)written,
                           " %u", (unsigned int)media->codecs[index].payload_type);
      if (extension < 0 || (size_t)extension >= sizeof(line) - (size_t)written)
        return AULA_STATUS_INTERNAL_ERROR;
      written += extension;
    }
  }
  if ((size_t)written + 2U >= sizeof(line)) return AULA_STATUS_INTERNAL_ERROR;
  line[written++] = '\r';
  line[written++] = '\n';
  line[written] = '\0';
  return aula_sdp_append(output, line);
}

static void aula_sdp_encode_srtp_key_salt(const aula_sdp_srtp_material *material,
                                           char output[41]) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t input = 0U;
  size_t encoded = 0U;
  while (input < AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES) {
    uint32_t bits = ((uint32_t)material->master_key_salt[input] << 16U) |
                    ((uint32_t)material->master_key_salt[input + 1U] << 8U) |
                    (uint32_t)material->master_key_salt[input + 2U];
    output[encoded++] = alphabet[(bits >> 18U) & 0x3fU];
    output[encoded++] = alphabet[(bits >> 12U) & 0x3fU];
    output[encoded++] = alphabet[(bits >> 6U) & 0x3fU];
    output[encoded++] = alphabet[bits & 0x3fU];
    input += 3U;
  }
  output[encoded] = '\0';
}

static aula_status aula_sdp_append_crypto(const aula_sdp_stored_media *media,
                                             aula_mutable_bytes *output) {
  char encoded[41];
  char line[AULA_SDP_MAX_LINE_BYTES];
  int written;
  aula_status status;
  if (media->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP) return AULA_STATUS_OK;
  if (media->value.profile != AULA_SDP_MEDIA_PROFILE_RTP_SAVP ||
      !media->srtp.present || media->srtp.crypto_tag == 0U ||
      media->srtp.lifetime == 0U ||
      media->srtp.lifetime > AULA_SDP_SRTP_DEFAULT_LIFETIME)
    return AULA_STATUS_INVALID_DATA;
  aula_sdp_encode_srtp_key_salt(&media->srtp.material, encoded);
  if (media->srtp.lifetime == AULA_SDP_SRTP_DEFAULT_LIFETIME) {
    written = snprintf(line, sizeof(line),
        "a=crypto:%u AES_CM_128_HMAC_SHA1_80 inline:%s\r\n",
        (unsigned int)media->srtp.crypto_tag, encoded);
  } else {
    written = snprintf(line, sizeof(line),
        "a=crypto:%u AES_CM_128_HMAC_SHA1_80 inline:%s|%" PRIu64 "\r\n",
        (unsigned int)media->srtp.crypto_tag, encoded,
        media->srtp.lifetime);
  }
  aula_sdp_secure_zero(encoded, sizeof(encoded));
  if (written < 0 || (size_t)written >= sizeof(line)) {
    aula_sdp_secure_zero(line, sizeof(line));
    return AULA_STATUS_INTERNAL_ERROR;
  }
  status = aula_sdp_append(output, line);
  aula_sdp_secure_zero(line, sizeof(line));
  return status;
}

static aula_status aula_sdp_append_media_transport(const aula_sdp_stored_media *media,
                                                      const char *direction,
                                                      aula_mutable_bytes *output) {
  char line[AULA_SDP_MAX_LINE_BYTES];
  int written;
  if (media->value.rtcp_mux) {
    if (aula_sdp_append(output, "a=rtcp-mux\r\n") != AULA_STATUS_OK)
      return AULA_STATUS_LIMIT_EXCEEDED;
  } else {
    written = snprintf(line, sizeof(line), "a=rtcp:%u\r\n", (unsigned int)media->value.rtcp_port);
    if (written < 0 || (size_t)written >= sizeof(line) ||
        aula_sdp_append(output, line) != AULA_STATUS_OK) return AULA_STATUS_LIMIT_EXCEEDED;
  }
  written = snprintf(line, sizeof(line), "a=%s\r\n", direction);
  if (written < 0 || (size_t)written >= sizeof(line) ||
      aula_sdp_append(output, line) != AULA_STATUS_OK) return AULA_STATUS_LIMIT_EXCEEDED;
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_append_codec(const aula_sdp_codec *codec,
                                           aula_mutable_bytes *output) {
  const char *codec_name = aula_sdp_codec_name(codec->kind);
  char line[AULA_SDP_MAX_LINE_BYTES];
  int written;
  if (codec_name == NULL) return AULA_STATUS_INVALID_DATA;
  written = snprintf(line, sizeof(line), "a=rtpmap:%u %s/%u\r\n",
                     (unsigned int)codec->payload_type, codec_name,
                     (unsigned int)codec->clock_rate);
  if (written < 0 || (size_t)written >= sizeof(line) ||
      aula_sdp_append(output, line) != AULA_STATUS_OK) return AULA_STATUS_LIMIT_EXCEEDED;
  if (codec->format_parameters[0] == '\0') return AULA_STATUS_OK;
  written = snprintf(line, sizeof(line), "a=fmtp:%u %s\r\n",
                     (unsigned int)codec->payload_type, codec->format_parameters);
  return written < 0 || (size_t)written >= sizeof(line) ||
                 aula_sdp_append(output, line) != AULA_STATUS_OK ?
             AULA_STATUS_LIMIT_EXCEEDED : AULA_STATUS_OK;
}

static aula_status aula_sdp_append_media_codecs(const aula_sdp_stored_media *media,
                                                   aula_mutable_bytes *output) {
  size_t index;
  int rank;
  for (rank = 0; rank < 4; ++rank) {
    for (index = 0U; index < media->value.codec_count; ++index) {
      aula_status status;
      if (aula_sdp_codec_order(media->codecs[index].kind) != rank) continue;
      status = aula_sdp_append_codec(&media->codecs[index], output);
      if (status != AULA_STATUS_OK) return status;
    }
  }
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_serialize_media(const aula_sdp_stored_media *media,
                                              const char *name,
                                              aula_mutable_bytes *output) {
  const char *direction = aula_sdp_direction_name(media->value.direction);
  aula_status status;
  if (direction == NULL || media->value.codec_count == 0U) return AULA_STATUS_INVALID_DATA;
  status = aula_sdp_append_media_description(media, name, output);
  if (status != AULA_STATUS_OK) return status == AULA_STATUS_INTERNAL_ERROR ? status :
                                  AULA_STATUS_LIMIT_EXCEEDED;
  status = aula_sdp_append_media_transport(media, direction, output);
  if (status == AULA_STATUS_OK) status = aula_sdp_append_crypto(media, output);
  if (status == AULA_STATUS_OK) status = aula_sdp_append_media_codecs(media, output);
  return status == AULA_STATUS_OK ? aula_sdp_serialize_feedback(media, output) : status;
}

aula_status aula_sdp_serialize(const aula_sdp_session *session,
                                 aula_mutable_bytes *output) {
  aula_status status;
  if (session == NULL || output == NULL || output->data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  output->length = 0U;
  if (!aula_sdp_valid_text(session->connection_address) ||
      aula_sdp_validate_local_ipv4(session->connection_address) != AULA_STATUS_OK) {
    return AULA_STATUS_INVALID_DATA;
  }
  status = aula_sdp_validate_media(&session->video, 1);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  status = aula_sdp_validate_media(&session->audio, 0);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  status = aula_sdp_append(output, "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=aula-sipd\r\n");
  if (status == AULA_STATUS_OK) {
    char line[AULA_SDP_MAX_LINE_BYTES];
    int written = snprintf(line, sizeof(line), "c=IN IP4 %s\r\n", session->connection_address);
    if (written < 0 || (size_t)written >= sizeof(line)) {
      status = AULA_STATUS_INTERNAL_ERROR;
    } else {
      status = aula_sdp_append(output, line);
    }
  }
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_append(output, "t=0 0\r\n");
  }
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_serialize_media(&session->video, "video", output);
  }
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_serialize_media(&session->audio, "audio", output);
  }
  return status;
}
