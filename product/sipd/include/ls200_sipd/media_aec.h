#ifndef LS200_SIPD_MEDIA_AEC_H
#define LS200_SIPD_MEDIA_AEC_H

#include "ls200_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_AEC_SAMPLE_RATE 8000U
#define LS200_AEC_FRAME_SAMPLES 160U
#define LS200_AEC_FRAME_BYTES (LS200_AEC_FRAME_SAMPLES * 2U)
#define LS200_AEC_MAX_DELAY_FRAMES 8U

typedef enum ls200_aec_delay_state {
  LS200_AEC_DELAY_INACTIVE = 0,
  LS200_AEC_DELAY_AVAILABLE,
  LS200_AEC_DELAY_UNCALIBRATED,
  LS200_AEC_DELAY_CALIBRATED
} ls200_aec_delay_state;

typedef struct ls200_aec_config {
  int enabled;
  uint32_t reference_delay_frames;
  int delay_calibrated;
} ls200_aec_config;

typedef struct ls200_aec_status {
  ls200_aec_delay_state delay_state;
  int available;
  int active;
  uint64_t processed_frames;
  uint64_t bypassed_frames;
  uint64_t reference_underflows;
  uint64_t resets;
} ls200_aec_status;

typedef struct ls200_aec ls200_aec;

int ls200_aec_native_available(void);
ls200_status ls200_aec_create(const ls200_aec_config *config, ls200_aec **out_aec);
ls200_status ls200_aec_push_reference(ls200_aec *aec, ls200_bytes pcm_s16le_8k_mono);
/* On unavailable reference or native support, output is a truthful bounded
 * bypass copy and AGAIN is returned. */
ls200_status ls200_aec_process_near(ls200_aec *aec, ls200_bytes near_s16le_8k_mono,
                                    ls200_mutable_bytes *out_pcm);
ls200_status ls200_aec_get_status(const ls200_aec *aec, ls200_aec_status *out_status);
void ls200_aec_reset(ls200_aec *aec);
void ls200_aec_destroy(ls200_aec *aec);

#ifdef __cplusplus
}
#endif

#endif
