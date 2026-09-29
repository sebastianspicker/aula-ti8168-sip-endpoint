#ifndef AULA_SIPD_MEDIA_AEC_H
#define AULA_SIPD_MEDIA_AEC_H

#include "aula_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_AEC_SAMPLE_RATE 8000U
#define AULA_AEC_FRAME_SAMPLES 160U
#define AULA_AEC_FRAME_BYTES (AULA_AEC_FRAME_SAMPLES * 2U)
#define AULA_AEC_MAX_DELAY_FRAMES 8U

typedef enum aula_aec_delay_state {
  AULA_AEC_DELAY_INACTIVE = 0,
  AULA_AEC_DELAY_AVAILABLE,
  AULA_AEC_DELAY_UNCALIBRATED,
  AULA_AEC_DELAY_CALIBRATED
} aula_aec_delay_state;

typedef struct aula_aec_config {
  int enabled;
  uint32_t reference_delay_frames;
  int delay_calibrated;
} aula_aec_config;

typedef struct aula_aec_status {
  aula_aec_delay_state delay_state;
  int available;
  int active;
  uint64_t processed_frames;
  uint64_t bypassed_frames;
  uint64_t reference_underflows;
  uint64_t resets;
} aula_aec_status;

typedef struct aula_aec aula_aec;

int aula_aec_native_available(void);
aula_status aula_aec_create(const aula_aec_config *config, aula_aec **out_aec);
aula_status aula_aec_push_reference(aula_aec *aec, aula_bytes pcm_s16le_8k_mono);
/* On unavailable reference or native support, output is a truthful bounded
 * bypass copy and AGAIN is returned. */
aula_status aula_aec_process_near(aula_aec *aec, aula_bytes near_s16le_8k_mono,
                                    aula_mutable_bytes *out_pcm);
aula_status aula_aec_get_status(const aula_aec *aec, aula_aec_status *out_status);
void aula_aec_reset(aula_aec *aec);
void aula_aec_destroy(aula_aec *aec);

#ifdef __cplusplus
}
#endif

#endif
