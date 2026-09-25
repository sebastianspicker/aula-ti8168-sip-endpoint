#include "control_private.h"

#include <string.h>

static uint64_t rotate_left(uint64_t value, unsigned int shift) {
  return (value << shift) | (value >> (64U - shift));
}

static uint64_t load_u64(const uint8_t input[8]) {
  uint64_t value = 0U;
  unsigned int index;
  for (index = 0U; index < 8U; ++index)
    value |= (uint64_t)input[index] << (index * 8U);
  return value;
}

#define SIP_ROUND() do { \
  v0 += v1; v1 = rotate_left(v1, 13U); v1 ^= v0; v0 = rotate_left(v0, 32U); \
  v2 += v3; v3 = rotate_left(v3, 16U); v3 ^= v2; \
  v0 += v3; v3 = rotate_left(v3, 21U); v3 ^= v0; \
  v2 += v1; v1 = rotate_left(v1, 17U); v1 ^= v2; v2 = rotate_left(v2, 32U); \
} while (0)

static uint64_t siphash24(const uint8_t key[16], ls200_bytes input,
                          uint64_t domain) {
  uint64_t k0 = load_u64(key), k1 = load_u64(key + 8U);
  uint64_t v0 = UINT64_C(0x736f6d6570736575) ^ k0 ^ domain;
  uint64_t v1 = UINT64_C(0x646f72616e646f6d) ^ k1;
  uint64_t v2 = UINT64_C(0x6c7967656e657261) ^ k0;
  uint64_t v3 = UINT64_C(0x7465646279746573) ^ k1 ^ domain;
  uint64_t final = (uint64_t)input.length << 56U;
  size_t offset = 0U, remaining;
  while (input.length - offset >= 8U) {
    uint64_t message = load_u64(input.data + offset);
    v3 ^= message; SIP_ROUND(); SIP_ROUND(); v0 ^= message; offset += 8U;
  }
  remaining = input.length - offset;
  while (remaining-- > 0U)
    final |= (uint64_t)input.data[offset + remaining] << (remaining * 8U);
  v3 ^= final; SIP_ROUND(); SIP_ROUND(); v0 ^= final; v2 ^= UINT64_C(0xff);
  SIP_ROUND(); SIP_ROUND(); SIP_ROUND(); SIP_ROUND();
  return v0 ^ v1 ^ v2 ^ v3;
}

void ls200_control_request_fingerprint(
    const uint8_t key[16], ls200_bytes payload, uint64_t output[2]) {
  output[0] = siphash24(key, payload, UINT64_C(0));
  output[1] = siphash24(key, payload, UINT64_C(0xa5a5a5a5a5a5a5a5));
}
