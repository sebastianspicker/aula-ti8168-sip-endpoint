#ifndef AULA_CONSOLE_PREVIEW_READER_H
#define AULA_CONSOLE_PREVIEW_READER_H

#include "aula_sipd/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_PREVIEW_MOVIE_PATH "/movie"
#define AULA_PREVIEW_REQUEST_BYTES 1024U
#define AULA_PREVIEW_MIN_ACCESS_UNIT_BYTES 4096U

typedef enum aula_preview_reader_state {
  AULA_PREVIEW_READER_NEW = 0,
  AULA_PREVIEW_READER_OPTIONS,
  AULA_PREVIEW_READER_DESCRIBE,
  AULA_PREVIEW_READER_SETUP,
  AULA_PREVIEW_READER_PLAY,
  AULA_PREVIEW_READER_STREAMING,
  AULA_PREVIEW_READER_FAILED,
  AULA_PREVIEW_READER_CLOSED
} aula_preview_reader_state;

/* The target is a policy-provided binary IPv4 address and port. There is no
 * URI or path input: the aggregate RTSP path is always /movie. Callers must
 * not log the rendered request because PLAY contains the RTSP session ID. */
typedef struct aula_preview_reader_config {
  uint8_t target_ipv4[4];
  uint16_t target_port;
  uint32_t maximum_access_unit_bytes;
} aula_preview_reader_config;

/* All pointers are reader-owned and remain valid only until the next push,
 * drain, reset, or destroy call. timestamp_90khz is monotonic and relative to
 * the first emitted decoder-ready access unit. */
typedef struct aula_preview_access_unit {
  aula_bytes annex_b;
  aula_bytes sps;
  aula_bytes pps;
  uint64_t timestamp_90khz;
  uint32_t parameter_set_generation;
  int keyframe;
  int discontinuity;
} aula_preview_access_unit;

typedef struct aula_preview_reader aula_preview_reader;

aula_status aula_preview_reader_create(
    const aula_preview_reader_config *config,
    aula_preview_reader **out_reader);

/* Produces the initial OPTIONS request. The caller owns connection setup and
 * partial-write handling for its independent RTSP/TCP socket. */
aula_status aula_preview_reader_start(aula_preview_reader *reader,
                                        aula_mutable_bytes *out_request);

/* Pushes a bounded fragment from the TCP stream. One call produces at most one
 * request or access unit. Call drain with an empty input until AGAIN before
 * reading more bytes from the socket. */
aula_status aula_preview_reader_push(aula_preview_reader *reader,
                                       aula_bytes input,
                                       aula_mutable_bytes *out_request,
                                       aula_preview_access_unit *out_unit);
aula_status aula_preview_reader_drain(aula_preview_reader *reader,
                                        aula_mutable_bytes *out_request,
                                        aula_preview_access_unit *out_unit);

/* Marks a transport EOF. A reconnect requires a new reader, which guarantees
 * independent parser, RTP, parameter-set, and timeline state. */
aula_status aula_preview_reader_eof(aula_preview_reader *reader);
aula_preview_reader_state aula_preview_reader_get_state(
    const aula_preview_reader *reader);
void aula_preview_reader_destroy(aula_preview_reader *reader);

#ifdef __cplusplus
}
#endif

#endif
