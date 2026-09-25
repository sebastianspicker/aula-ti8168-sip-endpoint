#ifndef LS200_CONSOLE_PREVIEW_READER_H
#define LS200_CONSOLE_PREVIEW_READER_H

#include "ls200_sipd/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_PREVIEW_MOVIE_PATH "/movie"
#define LS200_PREVIEW_REQUEST_BYTES 1024U
#define LS200_PREVIEW_MIN_ACCESS_UNIT_BYTES 4096U

typedef enum ls200_preview_reader_state {
  LS200_PREVIEW_READER_NEW = 0,
  LS200_PREVIEW_READER_OPTIONS,
  LS200_PREVIEW_READER_DESCRIBE,
  LS200_PREVIEW_READER_SETUP,
  LS200_PREVIEW_READER_PLAY,
  LS200_PREVIEW_READER_STREAMING,
  LS200_PREVIEW_READER_FAILED,
  LS200_PREVIEW_READER_CLOSED
} ls200_preview_reader_state;

/* The target is a policy-provided binary IPv4 address and port. There is no
 * URI or path input: the aggregate RTSP path is always /movie. Callers must
 * not log the rendered request because PLAY contains the RTSP session ID. */
typedef struct ls200_preview_reader_config {
  uint8_t target_ipv4[4];
  uint16_t target_port;
  uint32_t maximum_access_unit_bytes;
} ls200_preview_reader_config;

/* All pointers are reader-owned and remain valid only until the next push,
 * drain, reset, or destroy call. timestamp_90khz is monotonic and relative to
 * the first emitted decoder-ready access unit. */
typedef struct ls200_preview_access_unit {
  ls200_bytes annex_b;
  ls200_bytes sps;
  ls200_bytes pps;
  uint64_t timestamp_90khz;
  uint32_t parameter_set_generation;
  int keyframe;
  int discontinuity;
} ls200_preview_access_unit;

typedef struct ls200_preview_reader ls200_preview_reader;

ls200_status ls200_preview_reader_create(
    const ls200_preview_reader_config *config,
    ls200_preview_reader **out_reader);

/* Produces the initial OPTIONS request. The caller owns connection setup and
 * partial-write handling for its independent RTSP/TCP socket. */
ls200_status ls200_preview_reader_start(ls200_preview_reader *reader,
                                        ls200_mutable_bytes *out_request);

/* Pushes a bounded fragment from the TCP stream. One call produces at most one
 * request or access unit. Call drain with an empty input until AGAIN before
 * reading more bytes from the socket. */
ls200_status ls200_preview_reader_push(ls200_preview_reader *reader,
                                       ls200_bytes input,
                                       ls200_mutable_bytes *out_request,
                                       ls200_preview_access_unit *out_unit);
ls200_status ls200_preview_reader_drain(ls200_preview_reader *reader,
                                        ls200_mutable_bytes *out_request,
                                        ls200_preview_access_unit *out_unit);

/* Marks a transport EOF. A reconnect requires a new reader, which guarantees
 * independent parser, RTP, parameter-set, and timeline state. */
ls200_status ls200_preview_reader_eof(ls200_preview_reader *reader);
ls200_preview_reader_state ls200_preview_reader_get_state(
    const ls200_preview_reader *reader);
void ls200_preview_reader_destroy(ls200_preview_reader *reader);

#ifdef __cplusplus
}
#endif

#endif
