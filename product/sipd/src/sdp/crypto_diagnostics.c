#include "sdp_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  AULA_SDP_DIAG_MEDIA_NONE = 0,
  AULA_SDP_DIAG_MEDIA_VIDEO = 1,
  AULA_SDP_DIAG_MEDIA_AUDIO = 2,
  AULA_SDP_DIAG_MEDIA_OTHER = 3
};

enum {
  AULA_SDP_DIAG_PROFILE_NONE = 0,
  AULA_SDP_DIAG_PROFILE_AVP = 1,
  AULA_SDP_DIAG_PROFILE_SAVP = 2,
  AULA_SDP_DIAG_PROFILE_OTHER = 3
};

enum {
  AULA_SDP_DIAG_SUITE_UNKNOWN = 0,
  AULA_SDP_DIAG_SUITE_AES_CM_128_SHA1_80 = 1,
  AULA_SDP_DIAG_SUITE_AES_CM_128_SHA1_32 = 2,
  AULA_SDP_DIAG_SUITE_AEAD_AES_128_GCM = 3,
  AULA_SDP_DIAG_SUITE_AEAD_AES_256_GCM = 4
};

enum {
  AULA_SDP_DIAG_UNENCRYPTED_SRTP = 1,
  AULA_SDP_DIAG_UNENCRYPTED_SRTCP = 2,
  AULA_SDP_DIAG_UNAUTHENTICATED_SRTP = 4
};

typedef struct aula_sdp_crypto_diagnostic {
  unsigned index;
  unsigned word_count;
  unsigned tag;
  unsigned key_length;
  uint64_t lifetime;
  uint64_t exponent;
  unsigned mki_length;
  unsigned session_flags;
  int media;
  int profile;
  int suite;
  int tag_numeric;
  int tag_valid;
  int prior_crypto_present;
  int inline_prefix;
  int lifetime_present;
  int lifetime_valid;
  int exponent_valid;
  int mki_present;
  int mki_length_valid;
} aula_sdp_crypto_diagnostic;

static char *aula_sdp_diag_next_word(char **cursor) {
  char *word;
  while (**cursor == ' ') ++*cursor;
  if (**cursor == '\0') return NULL;
  word = *cursor;
  while (**cursor != '\0' && **cursor != ' ') ++*cursor;
  if (**cursor == ' ') *(*cursor)++ = '\0';
  return word;
}

static int aula_sdp_diag_suite(const char *word) {
  if (word == NULL) return AULA_SDP_DIAG_SUITE_UNKNOWN;
  if (strcmp(word, "AES_CM_128_HMAC_SHA1_80") == 0)
    return AULA_SDP_DIAG_SUITE_AES_CM_128_SHA1_80;
  if (strcmp(word, "AES_CM_128_HMAC_SHA1_32") == 0)
    return AULA_SDP_DIAG_SUITE_AES_CM_128_SHA1_32;
  if (strcmp(word, "AEAD_AES_128_GCM") == 0)
    return AULA_SDP_DIAG_SUITE_AEAD_AES_128_GCM;
  if (strcmp(word, "AEAD_AES_256_GCM") == 0)
    return AULA_SDP_DIAG_SUITE_AEAD_AES_256_GCM;
  return AULA_SDP_DIAG_SUITE_UNKNOWN;
}

static unsigned aula_sdp_diag_session_flag(const char *word) {
  if (strcmp(word, "UNENCRYPTED_SRTP") == 0) return AULA_SDP_DIAG_UNENCRYPTED_SRTP;
  if (strcmp(word, "UNENCRYPTED_SRTCP") == 0) return AULA_SDP_DIAG_UNENCRYPTED_SRTCP;
  if (strcmp(word, "UNAUTHENTICATED_SRTP") == 0)
    return AULA_SDP_DIAG_UNAUTHENTICATED_SRTP;
  return 0U;
}

static void aula_sdp_diag_parse_lifetime(char *word,
                                          aula_sdp_crypto_diagnostic *summary) {
  uint64_t value;
  summary->lifetime_present = word != NULL && *word != '\0';
  if (!summary->lifetime_present) return;
  if (!aula_sdp_parse_srtp_lifetime(word, &value)) return;
  summary->lifetime_valid = 1;
  if (strncmp(word, "2^", 2U) == 0) {
    summary->exponent_valid = 1;
    summary->exponent = (uint64_t)strtoull(word + 2, NULL, 10);
  } else {
    summary->lifetime = value;
  }
}

static void aula_sdp_diag_parse_mki(char *word,
                                     aula_sdp_crypto_diagnostic *summary) {
  char *separator;
  uint32_t length;
  summary->mki_present = word != NULL && *word != '\0';
  if (!summary->mki_present) return;
  separator = strrchr(word, ':');
  if (separator != NULL && separator != word &&
      aula_sdp_parse_u32(separator + 1, &length)) {
    summary->mki_length_valid = 1;
    summary->mki_length = length;
  }
}

static void aula_sdp_diag_parse_key(char *word,
                                     aula_sdp_crypto_diagnostic *summary) {
  char *option;
  char *mki = NULL;
  char *encoded;
  if (word == NULL || strncmp(word, "inline:", 7U) != 0) return;
  summary->inline_prefix = 1;
  encoded = word + 7U;
  option = strchr(encoded, '|');
  if (option != NULL) {
    *option++ = '\0';
    mki = strchr(option, '|');
    if (mki != NULL) *mki++ = '\0';
    else if (strchr(option, ':') != NULL) {
      mki = option;
      option = NULL;
    }
  }
  summary->key_length = (unsigned)strlen(encoded);
  aula_sdp_diag_parse_lifetime(option, summary);
  aula_sdp_diag_parse_mki(mki, summary);
}

static void aula_sdp_diag_parse_words(char *value,
                                       aula_sdp_crypto_diagnostic *summary) {
  char *cursor = value;
  char *word;
  while ((word = aula_sdp_diag_next_word(&cursor)) != NULL) {
    unsigned position = summary->word_count++;
    if (position == 0U) {
      uint16_t tag;
      summary->tag_numeric = aula_sdp_parse_u16(word, &tag);
      if (summary->tag_numeric) summary->tag = tag;
      summary->tag_valid = summary->tag_numeric && tag > 0U && tag <= 255U;
    } else if (position == 1U) {
      summary->suite = aula_sdp_diag_suite(word);
    } else if (position == 2U) {
      aula_sdp_diag_parse_key(word, summary);
    } else {
      summary->session_flags |= aula_sdp_diag_session_flag(word);
    }
  }
}

static void aula_sdp_diag_log(const aula_sdp_crypto_diagnostic *summary) {
  (void)fprintf(stderr,
      "aula-sipd: sdp-crypto index=%u media=%d profile=%d suite=%d words=%u tag_numeric=%d tag_valid=%d tag=%u prior_crypto_present=%d srtp_compiled=%d inline_prefix=%d key_encoded_length=%u lifetime_present=%d lifetime_valid=%d lifetime=%" PRIu64 " exponent_valid=%d exponent=%" PRIu64 " mki_present=%d mki_length_valid=%d mki_length=%u session_flags=%u\n",
      summary->index, summary->media, summary->profile, summary->suite,
      summary->word_count, summary->tag_numeric, summary->tag_valid,
      summary->tag, summary->prior_crypto_present,
      aula_sdp_srtp_is_available(), summary->inline_prefix,
      summary->key_length, summary->lifetime_present,
      summary->lifetime_valid, summary->lifetime, summary->exponent_valid,
      summary->exponent, summary->mki_present, summary->mki_length_valid,
      summary->mki_length, summary->session_flags);
}

static void aula_sdp_diag_media(char *line, int *media, int *profile,
                                 unsigned *crypto_count) {
  char *cursor = line;
  char *kind = aula_sdp_diag_next_word(&cursor);
  char *port = aula_sdp_diag_next_word(&cursor);
  char *profile_word = aula_sdp_diag_next_word(&cursor);
  (void)port;
  *crypto_count = 0U;
  *media = strcmp(kind, "m=video") == 0 ? AULA_SDP_DIAG_MEDIA_VIDEO :
      (strcmp(kind, "m=audio") == 0 ? AULA_SDP_DIAG_MEDIA_AUDIO :
                                      AULA_SDP_DIAG_MEDIA_OTHER);
  *profile = profile_word == NULL ? AULA_SDP_DIAG_PROFILE_NONE :
      (strcmp(profile_word, "RTP/SAVP") == 0 ? AULA_SDP_DIAG_PROFILE_SAVP :
       (strcmp(profile_word, "RTP/AVP") == 0 ? AULA_SDP_DIAG_PROFILE_AVP :
                                               AULA_SDP_DIAG_PROFILE_OTHER));
}

static void aula_sdp_diag_crypto(char *value, unsigned index, int media,
                                  int profile, unsigned *crypto_count) {
  aula_sdp_crypto_diagnostic summary;
  (void)memset(&summary, 0, sizeof(summary));
  summary.index = index;
  summary.media = media;
  summary.profile = profile;
  summary.prior_crypto_present = *crypto_count > 0U;
  aula_sdp_diag_parse_words(value, &summary);
  aula_sdp_diag_log(&summary);
  ++*crypto_count;
}

void aula_sdp_log_crypto_diagnostics(aula_bytes input, aula_status status) {
  char *copy;
  char *line;
  char *next;
  unsigned crypto_count = 0U;
  unsigned crypto_index = 0U;
  int media = AULA_SDP_DIAG_MEDIA_NONE;
  int profile = AULA_SDP_DIAG_PROFILE_NONE;
  if (status == AULA_STATUS_OK || input.data == NULL || input.length == 0U ||
      input.length > AULA_SIPD_MAX_SDP_BYTES)
    return;
  copy = (char *)malloc(input.length + 1U);
  if (copy == NULL) return;
  (void)memcpy(copy, input.data, input.length);
  copy[input.length] = '\0';
  line = copy;
  while (line != NULL) {
    next = strchr(line, '\n');
    if (next != NULL) *next++ = '\0';
    if (*line != '\0' && line[strlen(line) - 1U] == '\r') line[strlen(line) - 1U] = '\0';
    if (strncmp(line, "m=", 2U) == 0)
      aula_sdp_diag_media(line, &media, &profile, &crypto_count);
    else if (strncmp(line, "a=crypto:", 9U) == 0)
      aula_sdp_diag_crypto(line + 9U, ++crypto_index, media, profile, &crypto_count);
    line = next;
  }
  (void)fflush(stderr);
  aula_sdp_secure_zero(copy, input.length + 1U);
  free(copy);
}
