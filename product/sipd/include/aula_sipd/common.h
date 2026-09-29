#ifndef AULA_SIPD_COMMON_H
#define AULA_SIPD_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_SIPD_VERSION_MAJOR 0U
#define AULA_SIPD_VERSION_MINOR 1U
#define AULA_SIPD_VERSION_PATCH 0U

#define AULA_SIPD_MAX_SIP_MESSAGE_BYTES 16384U
#define AULA_SIPD_MAX_SDP_BYTES 8192U
#define AULA_SIPD_MAX_RTP_PACKET_BYTES 1500U
#define AULA_SIPD_MAX_RTCP_PACKET_BYTES 1500U
#define AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES (2U * 1024U * 1024U)
#define AULA_SIPD_MAX_AUDIO_FRAME_BYTES 4096U
#define AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES 2048U
#define AULA_SIPD_MAX_CORRELATION_ID_BYTES 33U

typedef enum aula_status {
  AULA_STATUS_OK = 0,
  AULA_STATUS_AGAIN = 1,
  AULA_STATUS_END = 2,
  AULA_STATUS_UNSUPPORTED = 3,
  /* The request was valid, but its optimistic-concurrency revision is stale. */
  AULA_STATUS_CONFLICT = 4,
  /* A replace rename committed, but the parent-directory fsync failed.  The
   * new file is authoritative; only crash durability is unconfirmed. */
  AULA_STATUS_PERSISTENCE_UNCERTAIN = 5,
  AULA_STATUS_INVALID_ARGUMENT = -1,
  AULA_STATUS_INVALID_DATA = -2,
  AULA_STATUS_LIMIT_EXCEEDED = -3,
  AULA_STATUS_CONFIGURATION_ERROR = -4,
  AULA_STATUS_PERMISSION_DENIED = -5,
  AULA_STATUS_IO_ERROR = -6,
  AULA_STATUS_TIMEOUT = -7,
  AULA_STATUS_STATE_ERROR = -8,
  AULA_STATUS_SECURITY_ERROR = -9,
  AULA_STATUS_INTERNAL_ERROR = -10
} aula_status;

typedef struct aula_bytes {
  const uint8_t *data;
  size_t length;
} aula_bytes;

typedef struct aula_mutable_bytes {
  uint8_t *data;
  size_t capacity;
  size_t length;
} aula_mutable_bytes;

typedef struct aula_deadline {
  uint64_t monotonic_ns;
} aula_deadline;

#ifdef __cplusplus
}
#endif

#endif
