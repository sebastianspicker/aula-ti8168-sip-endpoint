#include "sdp_internal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ls200_sdp_direction_name(ls200_sdp_direction direction) {
  switch (direction) {
    case LS200_SDP_SENDRECV: return "sendrecv";
    case LS200_SDP_SENDONLY: return "sendonly";
    case LS200_SDP_RECVONLY: return "recvonly";
    case LS200_SDP_INACTIVE: return "inactive";
    default: return NULL;
  }
}

static const char *ls200_sdp_codec_name(ls200_sdp_codec_kind kind) {
  switch (kind) {
    case LS200_SDP_CODEC_H264: return "H264";
    case LS200_SDP_CODEC_PCMU: return "PCMU";
    case LS200_SDP_CODEC_PCMA: return "PCMA";
    case LS200_SDP_CODEC_TELEPHONE_EVENT: return "telephone-event";
    default: return NULL;
  }
}

static int ls200_sdp_codec_order(ls200_sdp_codec_kind kind) {
  switch (kind) {
    case LS200_SDP_CODEC_H264: return 0;
    case LS200_SDP_CODEC_PCMU: return 1;
    case LS200_SDP_CODEC_PCMA: return 2;
    case LS200_SDP_CODEC_TELEPHONE_EVENT: return 3;
    default: return -1;
  }
}

static ls200_status ls200_sdp_append(ls200_mutable_bytes *output, const char *text) {
  size_t length = strlen(text);
  if (output->length > output->capacity || length > output->capacity - output->length) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  (void)memcpy(output->data + output->length, text, length);
  output->length += length;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_append_media_description(const ls200_sdp_stored_media *media,
                                                        const char *name,
                                                        ls200_mutable_bytes *output) {
  size_t index;
  int rank;
  char line[LS200_SDP_MAX_LINE_BYTES];
  const char *profile = media->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_AVP ? "RTP/AVP" :
                        media->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_SAVP ? "RTP/SAVP" :
                                                                                     NULL;
  int written;
  if (profile == NULL) return LS200_STATUS_INVALID_DATA;
  written = snprintf(line, sizeof(line), "m=%s %u %s", name,
                     (unsigned int)media->value.rtp_port, profile);
  if (written < 0 || (size_t)written >= sizeof(line)) return LS200_STATUS_INTERNAL_ERROR;
  for (rank = 0; rank < 4; ++rank) {
    for (index = 0U; index < media->value.codec_count; ++index) {
      int extension;
      if (ls200_sdp_codec_order(media->codecs[index].kind) != rank) continue;
      extension = snprintf(line + (size_t)written, sizeof(line) - (size_t)written,
                           " %u", (unsigned int)media->codecs[index].payload_type);
      if (extension < 0 || (size_t)extension >= sizeof(line) - (size_t)written)
        return LS200_STATUS_INTERNAL_ERROR;
      written += extension;
    }
  }
  if ((size_t)written + 2U >= sizeof(line)) return LS200_STATUS_INTERNAL_ERROR;
  line[written++] = '\r';
  line[written++] = '\n';
  line[written] = '\0';
  return ls200_sdp_append(output, line);
}

static void ls200_sdp_encode_srtp_key_salt(const ls200_sdp_srtp_material *material,
                                           char output[41]) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t input = 0U;
  size_t encoded = 0U;
  while (input < LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES) {
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

static ls200_status ls200_sdp_append_crypto(const ls200_sdp_stored_media *media,
                                             ls200_mutable_bytes *output) {
  char encoded[41];
  char line[LS200_SDP_MAX_LINE_BYTES];
  int written;
  ls200_status status;
  if (media->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_AVP) return LS200_STATUS_OK;
  if (media->value.profile != LS200_SDP_MEDIA_PROFILE_RTP_SAVP ||
      !media->srtp.present || media->srtp.crypto_tag == 0U ||
      media->srtp.lifetime == 0U ||
      media->srtp.lifetime > LS200_SDP_SRTP_DEFAULT_LIFETIME)
    return LS200_STATUS_INVALID_DATA;
  ls200_sdp_encode_srtp_key_salt(&media->srtp.material, encoded);
  if (media->srtp.lifetime == LS200_SDP_SRTP_DEFAULT_LIFETIME) {
    written = snprintf(line, sizeof(line),
        "a=crypto:%u AES_CM_128_HMAC_SHA1_80 inline:%s\r\n",
        (unsigned int)media->srtp.crypto_tag, encoded);
  } else {
    written = snprintf(line, sizeof(line),
        "a=crypto:%u AES_CM_128_HMAC_SHA1_80 inline:%s|%" PRIu64 "\r\n",
        (unsigned int)media->srtp.crypto_tag, encoded,
        media->srtp.lifetime);
  }
  ls200_sdp_secure_zero(encoded, sizeof(encoded));
  if (written < 0 || (size_t)written >= sizeof(line)) {
    ls200_sdp_secure_zero(line, sizeof(line));
    return LS200_STATUS_INTERNAL_ERROR;
  }
  status = ls200_sdp_append(output, line);
  ls200_sdp_secure_zero(line, sizeof(line));
  return status;
}

static ls200_status ls200_sdp_append_media_transport(const ls200_sdp_stored_media *media,
                                                      const char *direction,
                                                      ls200_mutable_bytes *output) {
  char line[LS200_SDP_MAX_LINE_BYTES];
  int written;
  if (media->value.rtcp_mux) {
    if (ls200_sdp_append(output, "a=rtcp-mux\r\n") != LS200_STATUS_OK)
      return LS200_STATUS_LIMIT_EXCEEDED;
  } else {
    written = snprintf(line, sizeof(line), "a=rtcp:%u\r\n", (unsigned int)media->value.rtcp_port);
    if (written < 0 || (size_t)written >= sizeof(line) ||
        ls200_sdp_append(output, line) != LS200_STATUS_OK) return LS200_STATUS_LIMIT_EXCEEDED;
  }
  written = snprintf(line, sizeof(line), "a=%s\r\n", direction);
  if (written < 0 || (size_t)written >= sizeof(line) ||
      ls200_sdp_append(output, line) != LS200_STATUS_OK) return LS200_STATUS_LIMIT_EXCEEDED;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_append_codec(const ls200_sdp_codec *codec,
                                           ls200_mutable_bytes *output) {
  const char *codec_name = ls200_sdp_codec_name(codec->kind);
  char line[LS200_SDP_MAX_LINE_BYTES];
  int written;
  if (codec_name == NULL) return LS200_STATUS_INVALID_DATA;
  written = snprintf(line, sizeof(line), "a=rtpmap:%u %s/%u\r\n",
                     (unsigned int)codec->payload_type, codec_name,
                     (unsigned int)codec->clock_rate);
  if (written < 0 || (size_t)written >= sizeof(line) ||
      ls200_sdp_append(output, line) != LS200_STATUS_OK) return LS200_STATUS_LIMIT_EXCEEDED;
  if (codec->format_parameters[0] == '\0') return LS200_STATUS_OK;
  written = snprintf(line, sizeof(line), "a=fmtp:%u %s\r\n",
                     (unsigned int)codec->payload_type, codec->format_parameters);
  return written < 0 || (size_t)written >= sizeof(line) ||
                 ls200_sdp_append(output, line) != LS200_STATUS_OK ?
             LS200_STATUS_LIMIT_EXCEEDED : LS200_STATUS_OK;
}

static ls200_status ls200_sdp_append_media_codecs(const ls200_sdp_stored_media *media,
                                                   ls200_mutable_bytes *output) {
  size_t index;
  int rank;
  for (rank = 0; rank < 4; ++rank) {
    for (index = 0U; index < media->value.codec_count; ++index) {
      ls200_status status;
      if (ls200_sdp_codec_order(media->codecs[index].kind) != rank) continue;
      status = ls200_sdp_append_codec(&media->codecs[index], output);
      if (status != LS200_STATUS_OK) return status;
    }
  }
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_serialize_media(const ls200_sdp_stored_media *media,
                                              const char *name,
                                              ls200_mutable_bytes *output) {
  const char *direction = ls200_sdp_direction_name(media->value.direction);
  ls200_status status;
  if (direction == NULL || media->value.codec_count == 0U) return LS200_STATUS_INVALID_DATA;
  status = ls200_sdp_append_media_description(media, name, output);
  if (status != LS200_STATUS_OK) return status == LS200_STATUS_INTERNAL_ERROR ? status :
                                  LS200_STATUS_LIMIT_EXCEEDED;
  status = ls200_sdp_append_media_transport(media, direction, output);
  if (status == LS200_STATUS_OK) status = ls200_sdp_append_crypto(media, output);
  if (status == LS200_STATUS_OK) status = ls200_sdp_append_media_codecs(media, output);
  return status == LS200_STATUS_OK ? ls200_sdp_serialize_feedback(media, output) : status;
}

ls200_status ls200_sdp_serialize(const ls200_sdp_session *session,
                                 ls200_mutable_bytes *output) {
  ls200_status status;
  if (session == NULL || output == NULL || output->data == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  output->length = 0U;
  if (!ls200_sdp_valid_text(session->connection_address) ||
      ls200_sdp_validate_local_ipv4(session->connection_address) != LS200_STATUS_OK) {
    return LS200_STATUS_INVALID_DATA;
  }
  status = ls200_sdp_validate_media(&session->video, 1);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  status = ls200_sdp_validate_media(&session->audio, 0);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  status = ls200_sdp_append(output, "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=ls200-sipd\r\n");
  if (status == LS200_STATUS_OK) {
    char line[LS200_SDP_MAX_LINE_BYTES];
    int written = snprintf(line, sizeof(line), "c=IN IP4 %s\r\n", session->connection_address);
    if (written < 0 || (size_t)written >= sizeof(line)) {
      status = LS200_STATUS_INTERNAL_ERROR;
    } else {
      status = ls200_sdp_append(output, line);
    }
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_append(output, "t=0 0\r\n");
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_serialize_media(&session->video, "video", output);
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_serialize_media(&session->audio, "audio", output);
  }
  return status;
}
