#include "sdp_internal.h"

#include <string.h>

int aula_sdp_srtp_is_available(void) {
#if AULA_SIPD_HAVE_SRTP
  return 1;
#else
  return 0;
#endif
}

int aula_sdp_srtp_policy_is_valid(const aula_sdp_srtp_policy *policy) {
  return policy != NULL && policy->crypto_tag != 0U;
}

aula_status aula_sdp_assign_srtp_policy(aula_sdp_stored_media *media,
                                          const aula_sdp_srtp_policy *policy) {
  if (media == NULL || !aula_sdp_srtp_is_available()) return AULA_STATUS_UNSUPPORTED;
  if (!aula_sdp_srtp_policy_is_valid(policy)) return AULA_STATUS_INVALID_ARGUMENT;
  media->value.profile = AULA_SDP_MEDIA_PROFILE_RTP_SAVP;
  media->srtp.present = 1;
  media->srtp.crypto_tag = policy->crypto_tag;
  media->srtp.lifetime = AULA_SDP_SRTP_DEFAULT_LIFETIME;
  (void)memcpy(&media->srtp.material, &policy->outbound_material,
               sizeof(media->srtp.material));
  return AULA_STATUS_OK;
}

static int aula_sdp_parse_u64(const char *text, uint64_t *out_value) {
  uint64_t value = 0U;
  const char *cursor;
  if (text == NULL || out_value == NULL || *text == '\0' || *text == '-') return 0;
  for (cursor = text; *cursor != '\0'; ++cursor) {
    uint64_t digit;
    if (*cursor < '0' || *cursor > '9') return 0;
    digit = (uint64_t)(*cursor - '0');
    if (value > (UINT64_MAX - digit) / 10U) return 0;
    value = value * 10U + digit;
  }
  *out_value = value;
  return 1;
}

int aula_sdp_parse_srtp_lifetime(const char *text, uint64_t *out_lifetime) {
  uint64_t value;
  const char *number = text;
  int exponent = 0;
  if (text == NULL || out_lifetime == NULL) return 0;
  if (strncmp(text, "2^", 2U) == 0) {
    exponent = 1;
    number = text + 2U;
  }
  if (*number == '\0' || (number[0] == '0' && number[1] != '\0') ||
      !aula_sdp_parse_u64(number, &value)) return 0;
  if (exponent != 0) {
    if (value > 48U) return 0;
    value = UINT64_C(1) << (unsigned)value;
  }
  if (value == 0U || value > AULA_SDP_SRTP_DEFAULT_LIFETIME) return 0;
  *out_lifetime = value;
  return 1;
}

static int aula_sdp_base64_value(char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

static aula_status aula_sdp_decode_srtp_key_salt(
    const char *encoded, aula_sdp_srtp_material *material) {
  size_t index;
  size_t output = 0U;
  if (encoded == NULL || material == NULL ||
      strlen(encoded) != AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES / 3U * 4U)
    return AULA_STATUS_INVALID_DATA;
  for (index = 0U; index < AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES / 3U * 4U;
       index += 4U) {
    int first = aula_sdp_base64_value(encoded[index]);
    int second = aula_sdp_base64_value(encoded[index + 1U]);
    int third = aula_sdp_base64_value(encoded[index + 2U]);
    int fourth = aula_sdp_base64_value(encoded[index + 3U]);
    uint32_t bits;
    if (first < 0 || second < 0 || third < 0 || fourth < 0)
      return AULA_STATUS_INVALID_DATA;
    bits = ((uint32_t)first << 18U) | ((uint32_t)second << 12U) |
           ((uint32_t)third << 6U) | (uint32_t)fourth;
    material->master_key_salt[output++] = (uint8_t)(bits >> 16U);
    material->master_key_salt[output++] = (uint8_t)(bits >> 8U);
    material->master_key_salt[output++] = (uint8_t)bits;
  }
  return output == AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES ? AULA_STATUS_OK :
                                                          AULA_STATUS_INTERNAL_ERROR;
}

static aula_status aula_sdp_parse_crypto_key(
    char *word, aula_sdp_stored_srtp *srtp) {
  char *encoded;
  char *lifetime;
  if (word == NULL || srtp == NULL || strncmp(word, "inline:", 7U) != 0)
    return AULA_STATUS_INVALID_DATA;
  encoded = word + 7U;
  lifetime = strchr(encoded, '|');
  srtp->lifetime = AULA_SDP_SRTP_DEFAULT_LIFETIME;
  if (lifetime != NULL) {
    *lifetime++ = '\0';
    if (strchr(lifetime, '|') != NULL || strchr(lifetime, ':') != NULL ||
        !aula_sdp_parse_srtp_lifetime(lifetime, &srtp->lifetime))
      return AULA_STATUS_INVALID_DATA;
  }
  return aula_sdp_decode_srtp_key_salt(encoded, &srtp->material);
}

aula_status aula_sdp_parse_crypto(aula_sdp_stored_media *media, char *value) {
  char *words[4];
  size_t count;
  uint16_t tag;
  if (media == NULL || value == NULL ||
      media->value.profile != AULA_SDP_MEDIA_PROFILE_RTP_SAVP ||
      media->srtp.present || !aula_sdp_srtp_is_available() ||
      !aula_sdp_split_words(value, words, 4U, &count) || count != 3U ||
      words[0][0] == '0' || !aula_sdp_parse_u16(words[0], &tag) ||
      tag == 0U || tag > 255U ||
      strcmp(words[1], "AES_CM_128_HMAC_SHA1_80") != 0)
    return AULA_STATUS_INVALID_DATA;
  if (aula_sdp_parse_crypto_key(words[2], &media->srtp) != AULA_STATUS_OK) {
    aula_sdp_secure_zero(&media->srtp, sizeof(media->srtp));
    return AULA_STATUS_INVALID_DATA;
  }
  media->srtp.present = 1;
  media->srtp.crypto_tag = (uint8_t)tag;
  return AULA_STATUS_OK;
}
