#ifndef LS200_CONSOLE_PREVIEW_FLV_H
#define LS200_CONSOLE_PREVIEW_FLV_H

#include "preview_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_preview_flv_error {
  LS200_PREVIEW_FLV_ERROR_NONE = 0,
  LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT,
  LS200_PREVIEW_FLV_ERROR_UNSUPPORTED_TIMING,
  LS200_PREVIEW_FLV_ERROR_WRITE_FAILED
} ls200_preview_flv_error;

/* A sink must consume the complete span before returning OK. Any other status
 * is terminal for the HTTP-FLV response because a partial FLV tag cannot be
 * repaired. */
typedef ls200_status (*ls200_preview_write_fn)(void *context,
                                               ls200_bytes bytes);

typedef struct ls200_preview_flv_mux {
  uint64_t first_timestamp_90khz;
  uint64_t last_timestamp_90khz;
  uint32_t parameter_set_generation;
  ls200_preview_flv_error last_error;
  int header_written;
  int decoder_config_written;
} ls200_preview_flv_mux;

void ls200_preview_flv_mux_init(ls200_preview_flv_mux *mux);

/* Emits a video-only FLV stream. The first accepted unit, and every unit after
 * a discontinuity or parameter-set change, must be an IDR. SPS/PPS are emitted
 * only in AVCDecoderConfigurationRecord tags. Every other NAL byte is copied
 * unchanged behind a four-byte big-endian length. */
ls200_status ls200_preview_flv_mux_write(
    ls200_preview_flv_mux *mux,
    const ls200_preview_access_unit *unit,
    ls200_preview_write_fn write_fn,
    void *write_context);

ls200_preview_flv_error ls200_preview_flv_mux_last_error(
    const ls200_preview_flv_mux *mux);

#ifdef __cplusplus
}
#endif

#endif
