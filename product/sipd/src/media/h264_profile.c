#include "h264_profile_private.h"

#include <stddef.h>
#include <stdint.h>

typedef enum aula_h264_subprofile {
  AULA_H264_SUBPROFILE_UNKNOWN = 0,
  AULA_H264_SUBPROFILE_CONSTRAINED_BASELINE,
  AULA_H264_SUBPROFILE_BASELINE,
  AULA_H264_SUBPROFILE_MAIN,
  AULA_H264_SUBPROFILE_EXTENDED,
  AULA_H264_SUBPROFILE_HIGH,
  AULA_H264_SUBPROFILE_HIGH_10,
  AULA_H264_SUBPROFILE_HIGH_422,
  AULA_H264_SUBPROFILE_HIGH_444,
  AULA_H264_SUBPROFILE_HIGH_10_INTRA,
  AULA_H264_SUBPROFILE_HIGH_422_INTRA,
  AULA_H264_SUBPROFILE_HIGH_444_INTRA,
  AULA_H264_SUBPROFILE_CAVLC_444_INTRA
} aula_h264_subprofile;

static int h264_hex(char value, uint8_t *out) {
  if (value >= '0' && value <= '9') {
    *out = (uint8_t)(value - '0');
    return 1;
  }
  if (value >= 'a' && value <= 'f') {
    *out = (uint8_t)(value - 'a' + 10);
    return 1;
  }
  if (value >= 'A' && value <= 'F') {
    *out = (uint8_t)(value - 'A' + 10);
    return 1;
  }
  return 0;
}

static int h264_profile_bytes(const char text[7], uint8_t bytes[3]) {
  size_t index;
  if (text == NULL || bytes == NULL || text[6] != '\0') return 0;
  for (index = 0U; index < 3U; ++index) {
    uint8_t high;
    uint8_t low;
    if (!h264_hex(text[index * 2U], &high) ||
        !h264_hex(text[index * 2U + 1U], &low)) return 0;
    bytes[index] = (uint8_t)((high << 4U) | low);
  }
  return (bytes[1] & 0x03U) == 0U;
}

static aula_h264_subprofile h264_legacy_subprofile(uint8_t profile,
                                                    uint8_t constraints) {
  switch (profile) {
    case 0x42U:
      return (constraints & 0x40U) != 0U
                 ? AULA_H264_SUBPROFILE_CONSTRAINED_BASELINE
                 : AULA_H264_SUBPROFILE_BASELINE;
    case 0x4dU:
      if ((constraints & 0x80U) != 0U)
        return AULA_H264_SUBPROFILE_CONSTRAINED_BASELINE;
      return (constraints & 0x20U) == 0U
                 ? AULA_H264_SUBPROFILE_MAIN
                 : AULA_H264_SUBPROFILE_UNKNOWN;
    case 0x58U:
      if ((constraints & 0xc0U) == 0xc0U)
        return AULA_H264_SUBPROFILE_CONSTRAINED_BASELINE;
      if ((constraints & 0xc0U) == 0x80U)
        return AULA_H264_SUBPROFILE_BASELINE;
      return (constraints & 0xc0U) == 0U
                 ? AULA_H264_SUBPROFILE_EXTENDED
                 : AULA_H264_SUBPROFILE_UNKNOWN;
    default: return AULA_H264_SUBPROFILE_UNKNOWN;
  }
}

static aula_h264_subprofile h264_high_subprofile(uint8_t profile,
                                                    uint8_t constraints) {
  switch (profile) {
    case 0x64U:
      return constraints == 0U ? AULA_H264_SUBPROFILE_HIGH
                               : AULA_H264_SUBPROFILE_UNKNOWN;
    case 0x6eU:
      return constraints == 0U ? AULA_H264_SUBPROFILE_HIGH_10
           : constraints == 0x10U ? AULA_H264_SUBPROFILE_HIGH_10_INTRA
                                  : AULA_H264_SUBPROFILE_UNKNOWN;
    case 0x7aU:
      return constraints == 0U ? AULA_H264_SUBPROFILE_HIGH_422
           : constraints == 0x10U ? AULA_H264_SUBPROFILE_HIGH_422_INTRA
                                  : AULA_H264_SUBPROFILE_UNKNOWN;
    case 0xf4U:
      return constraints == 0U ? AULA_H264_SUBPROFILE_HIGH_444
           : constraints == 0x10U ? AULA_H264_SUBPROFILE_HIGH_444_INTRA
                                  : AULA_H264_SUBPROFILE_UNKNOWN;
    case 0x2cU:
      return constraints == 0x10U ? AULA_H264_SUBPROFILE_CAVLC_444_INTRA
                                  : AULA_H264_SUBPROFILE_UNKNOWN;
    default: return AULA_H264_SUBPROFILE_UNKNOWN;
  }
}

static aula_h264_subprofile h264_subprofile(uint8_t profile,
                                              uint8_t constraints) {
  if ((constraints & 0x0fU) != 0U) return AULA_H264_SUBPROFILE_UNKNOWN;
  if (profile == 0x42U || profile == 0x4dU || profile == 0x58U)
    return h264_legacy_subprofile(profile, constraints);
  return h264_high_subprofile(profile, constraints);
}

static int h264_level_1b(const uint8_t bytes[3]) {
  return (bytes[0] == 0x42U || bytes[0] == 0x4dU || bytes[0] == 0x58U) &&
         bytes[2] == 0x0bU && (bytes[1] & 0x10U) != 0U;
}

int aula_h264_profile_level_id_valid(const char profile_level_id[7]) {
  uint8_t bytes[3];
  return h264_profile_bytes(profile_level_id, bytes) &&
         h264_subprofile(bytes[0], bytes[1]) != AULA_H264_SUBPROFILE_UNKNOWN;
}

int aula_h264_profile_level_id_compatible(const char left[7],
                                           const char right[7]) {
  uint8_t left_bytes[3];
  uint8_t right_bytes[3];
  aula_h264_subprofile left_profile;
  aula_h264_subprofile right_profile;
  if (!h264_profile_bytes(left, left_bytes) ||
      !h264_profile_bytes(right, right_bytes)) return 0;
  left_profile = h264_subprofile(left_bytes[0], left_bytes[1]);
  right_profile = h264_subprofile(right_bytes[0], right_bytes[1]);
  return left_profile != AULA_H264_SUBPROFILE_UNKNOWN &&
         left_profile == right_profile && left_bytes[2] == right_bytes[2] &&
         h264_level_1b(left_bytes) == h264_level_1b(right_bytes);
}
