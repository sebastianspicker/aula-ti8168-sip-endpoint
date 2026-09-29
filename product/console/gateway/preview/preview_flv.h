#ifndef AULA_CONSOLE_PREVIEW_FLV_H
#define AULA_CONSOLE_PREVIEW_FLV_H

#include "preview_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_preview_flv_error {
  AULA_PREVIEW_FLV_ERROR_NONE = 0,
  AULA_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT,
  AULA_PREVIEW_FLV_ERROR_UNSUPPORTED_TIMING,
  AULA_PREVIEW_FLV_ERROR_WRITE_FAILED
} aula_preview_flv_error;

/* A sink must consume the complete span before returning OK. Any other status
 * is terminal for the HTTP-FLV response because a partial FLV tag cannot be
 * repaired. */
typedef aula_status (*aula_preview_write_fn)(void *context,
                                               aula_bytes bytes);

typedef struct aula_preview_flv_mux {
  uint64_t first_timestamp_90khz;
  uint64_t last_timestamp_90khz;
  uint32_t parameter_set_generation;
  aula_preview_flv_error last_error;
  int header_written;
  int decoder_config_written;
} aula_preview_flv_mux;

void aula_preview_flv_mux_init(aula_preview_flv_mux *mux);

/* Emits a video-only FLV stream. The first accepted unit, and every unit after
 * a discontinuity or parameter-set change, must be an IDR. SPS/PPS are emitted
 * only in AVCDecoderConfigurationRecord tags. Every other NAL byte is copied
 * unchanged behind a four-byte big-endian length. */
aula_status aula_preview_flv_mux_write(
    aula_preview_flv_mux *mux,
    const aula_preview_access_unit *unit,
    aula_preview_write_fn write_fn,
    void *write_context);

aula_preview_flv_error aula_preview_flv_mux_last_error(
    const aula_preview_flv_mux *mux);

#ifdef __cplusplus
}
#endif

#endif
