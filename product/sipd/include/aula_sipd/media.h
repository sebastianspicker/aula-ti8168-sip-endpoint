#ifndef AULA_SIPD_MEDIA_H
#define AULA_SIPD_MEDIA_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_media_kind {
  AULA_MEDIA_VIDEO_H264_ANNEX_B = 0,
  AULA_MEDIA_AUDIO_PCM_S16LE = 1
} aula_media_kind;

typedef struct aula_media_frame {
  aula_media_kind kind;
  aula_bytes data;
  uint64_t pts_ns;
  uint32_t sample_rate;
  uint8_t channels;
  int keyframe;
} aula_media_frame;

typedef struct aula_media_health {
  uint64_t frames_read;
  uint64_t frames_dropped;
  uint64_t bytes_read;
  uint64_t restarts;
  uint64_t last_success_ns;
  aula_status last_error;
} aula_media_health;

typedef struct aula_receive_shim_stats {
  uint64_t accepted_packets;
  uint64_t rejected_packets;
  uint64_t drained_bytes;
  uint64_t last_packet_ns;
  int rx_rendering;
} aula_receive_shim_stats;

/* Frame data is backend-owned and valid only until the next backend read. */
aula_status aula_media_validate_frame(const aula_media_frame *frame);
aula_status aula_media_g711_encode(aula_media_kind input_kind,
                                     aula_bytes pcm_s16le, uint32_t sample_rate,
                                     uint8_t channels, int use_pcma,
                                     aula_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
