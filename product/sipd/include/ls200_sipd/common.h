#ifndef LS200_SIPD_COMMON_H
#define LS200_SIPD_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_SIPD_VERSION_MAJOR 0U
#define LS200_SIPD_VERSION_MINOR 1U
#define LS200_SIPD_VERSION_PATCH 0U

#define LS200_SIPD_MAX_SIP_MESSAGE_BYTES 16384U
#define LS200_SIPD_MAX_SDP_BYTES 8192U
#define LS200_SIPD_MAX_RTP_PACKET_BYTES 1500U
#define LS200_SIPD_MAX_RTCP_PACKET_BYTES 1500U
#define LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES (2U * 1024U * 1024U)
#define LS200_SIPD_MAX_AUDIO_FRAME_BYTES 4096U
#define LS200_SIPD_MAX_CONTROL_MESSAGE_BYTES 2048U
#define LS200_SIPD_MAX_CORRELATION_ID_BYTES 33U

typedef enum ls200_status {
  LS200_STATUS_OK = 0,
  LS200_STATUS_AGAIN = 1,
  LS200_STATUS_END = 2,
  LS200_STATUS_UNSUPPORTED = 3,
  /* The request was valid, but its optimistic-concurrency revision is stale. */
  LS200_STATUS_CONFLICT = 4,
  /* A replace rename committed, but the parent-directory fsync failed.  The
   * new file is authoritative; only crash durability is unconfirmed. */
  LS200_STATUS_PERSISTENCE_UNCERTAIN = 5,
  LS200_STATUS_INVALID_ARGUMENT = -1,
  LS200_STATUS_INVALID_DATA = -2,
  LS200_STATUS_LIMIT_EXCEEDED = -3,
  LS200_STATUS_CONFIGURATION_ERROR = -4,
  LS200_STATUS_PERMISSION_DENIED = -5,
  LS200_STATUS_IO_ERROR = -6,
  LS200_STATUS_TIMEOUT = -7,
  LS200_STATUS_STATE_ERROR = -8,
  LS200_STATUS_SECURITY_ERROR = -9,
  LS200_STATUS_INTERNAL_ERROR = -10
} ls200_status;

typedef struct ls200_bytes {
  const uint8_t *data;
  size_t length;
} ls200_bytes;

typedef struct ls200_mutable_bytes {
  uint8_t *data;
  size_t capacity;
  size_t length;
} ls200_mutable_bytes;

typedef struct ls200_deadline {
  uint64_t monotonic_ns;
} ls200_deadline;

#ifdef __cplusplus
}
#endif

#endif
