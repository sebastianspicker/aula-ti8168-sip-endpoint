#include "sdp_internal.h"

#include <string.h>

int ls200_sdp_srtp_is_available(void) {
#if LS200_SIPD_HAVE_SRTP
  return 1;
#else
  return 0;
#endif
}

int ls200_sdp_srtp_policy_is_valid(const ls200_sdp_srtp_policy *policy) {
  return policy != NULL && policy->crypto_tag != 0U;
}

ls200_status ls200_sdp_assign_srtp_policy(ls200_sdp_stored_media *media,
                                          const ls200_sdp_srtp_policy *policy) {
  if (media == NULL || !ls200_sdp_srtp_is_available()) return LS200_STATUS_UNSUPPORTED;
  if (!ls200_sdp_srtp_policy_is_valid(policy)) return LS200_STATUS_INVALID_ARGUMENT;
  media->value.profile = LS200_SDP_MEDIA_PROFILE_RTP_SAVP;
  media->srtp.present = 1;
  media->srtp.crypto_tag = policy->crypto_tag;
  media->srtp.lifetime = LS200_SDP_SRTP_DEFAULT_LIFETIME;
  (void)memcpy(&media->srtp.material, &policy->outbound_material,
               sizeof(media->srtp.material));
  return LS200_STATUS_OK;
}

static int ls200_sdp_parse_u64(const char *text, uint64_t *out_value) {
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

int ls200_sdp_parse_srtp_lifetime(const char *text, uint64_t *out_lifetime) {
  uint64_t value;
  const char *number = text;
  int exponent = 0;
  if (text == NULL || out_lifetime == NULL) return 0;
  if (strncmp(text, "2^", 2U) == 0) {
    exponent = 1;
    number = text + 2U;
  }
  if (*number == '\0' || (number[0] == '0' && number[1] != '\0') ||
      !ls200_sdp_parse_u64(number, &value)) return 0;
  if (exponent != 0) {
    if (value > 48U) return 0;
    value = UINT64_C(1) << (unsigned)value;
  }
  if (value == 0U || value > LS200_SDP_SRTP_DEFAULT_LIFETIME) return 0;
  *out_lifetime = value;
  return 1;
}

static int ls200_sdp_base64_value(char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '+') return 62;
  if (character == '/') return 63;
  return -1;
}

static ls200_status ls200_sdp_decode_srtp_key_salt(
    const char *encoded, ls200_sdp_srtp_material *material) {
  size_t index;
  size_t output = 0U;
  if (encoded == NULL || material == NULL ||
      strlen(encoded) != LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES / 3U * 4U)
    return LS200_STATUS_INVALID_DATA;
  for (index = 0U; index < LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES / 3U * 4U;
       index += 4U) {
    int first = ls200_sdp_base64_value(encoded[index]);
    int second = ls200_sdp_base64_value(encoded[index + 1U]);
    int third = ls200_sdp_base64_value(encoded[index + 2U]);
    int fourth = ls200_sdp_base64_value(encoded[index + 3U]);
    uint32_t bits;
    if (first < 0 || second < 0 || third < 0 || fourth < 0)
      return LS200_STATUS_INVALID_DATA;
    bits = ((uint32_t)first << 18U) | ((uint32_t)second << 12U) |
           ((uint32_t)third << 6U) | (uint32_t)fourth;
    material->master_key_salt[output++] = (uint8_t)(bits >> 16U);
    material->master_key_salt[output++] = (uint8_t)(bits >> 8U);
    material->master_key_salt[output++] = (uint8_t)bits;
  }
  return output == LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES ? LS200_STATUS_OK :
                                                          LS200_STATUS_INTERNAL_ERROR;
}

static ls200_status ls200_sdp_parse_crypto_key(
    char *word, ls200_sdp_stored_srtp *srtp) {
  char *encoded;
  char *lifetime;
  if (word == NULL || srtp == NULL || strncmp(word, "inline:", 7U) != 0)
    return LS200_STATUS_INVALID_DATA;
  encoded = word + 7U;
  lifetime = strchr(encoded, '|');
  srtp->lifetime = LS200_SDP_SRTP_DEFAULT_LIFETIME;
  if (lifetime != NULL) {
    *lifetime++ = '\0';
    if (strchr(lifetime, '|') != NULL || strchr(lifetime, ':') != NULL ||
        !ls200_sdp_parse_srtp_lifetime(lifetime, &srtp->lifetime))
      return LS200_STATUS_INVALID_DATA;
  }
  return ls200_sdp_decode_srtp_key_salt(encoded, &srtp->material);
}

ls200_status ls200_sdp_parse_crypto(ls200_sdp_stored_media *media, char *value) {
  char *words[4];
  size_t count;
  uint16_t tag;
  if (media == NULL || value == NULL ||
      media->value.profile != LS200_SDP_MEDIA_PROFILE_RTP_SAVP ||
      media->srtp.present || !ls200_sdp_srtp_is_available() ||
      !ls200_sdp_split_words(value, words, 4U, &count) || count != 3U ||
      words[0][0] == '0' || !ls200_sdp_parse_u16(words[0], &tag) ||
      tag == 0U || tag > 255U ||
      strcmp(words[1], "AES_CM_128_HMAC_SHA1_80") != 0)
    return LS200_STATUS_INVALID_DATA;
  if (ls200_sdp_parse_crypto_key(words[2], &media->srtp) != LS200_STATUS_OK) {
    ls200_sdp_secure_zero(&media->srtp, sizeof(media->srtp));
    return LS200_STATUS_INVALID_DATA;
  }
  media->srtp.present = 1;
  media->srtp.crypto_tag = (uint8_t)tag;
  return LS200_STATUS_OK;
}
