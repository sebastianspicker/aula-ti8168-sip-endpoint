#ifndef LS200_SIPD_MEDIA_H
#define LS200_SIPD_MEDIA_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_media_kind {
  LS200_MEDIA_VIDEO_H264_ANNEX_B = 0,
  LS200_MEDIA_AUDIO_PCM_S16LE = 1
} ls200_media_kind;

typedef struct ls200_media_frame {
  ls200_media_kind kind;
  ls200_bytes data;
  uint64_t pts_ns;
  uint32_t sample_rate;
  uint8_t channels;
  int keyframe;
} ls200_media_frame;

typedef struct ls200_media_health {
  uint64_t frames_read;
  uint64_t frames_dropped;
  uint64_t bytes_read;
  uint64_t restarts;
  uint64_t last_success_ns;
  ls200_status last_error;
} ls200_media_health;

typedef struct ls200_receive_shim_stats {
  uint64_t accepted_packets;
  uint64_t rejected_packets;
  uint64_t drained_bytes;
  uint64_t last_packet_ns;
  int rx_rendering;
} ls200_receive_shim_stats;

/* Frame data is backend-owned and valid only until the next backend read. */
ls200_status ls200_media_validate_frame(const ls200_media_frame *frame);
ls200_status ls200_media_g711_encode(ls200_media_kind input_kind,
                                     ls200_bytes pcm_s16le, uint32_t sample_rate,
                                     uint8_t channels, int use_pcma,
                                     ls200_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
