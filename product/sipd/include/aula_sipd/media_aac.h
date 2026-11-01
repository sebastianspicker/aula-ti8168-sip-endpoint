#ifndef AULA_SIPD_MEDIA_AAC_H
#define AULA_SIPD_MEDIA_AAC_H

#include "aula_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_aac_config {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t audio_object_type;
} aula_aac_config;

typedef struct aula_aac_decoder aula_aac_decoder;

/* Parses MPEG-4 AudioSpecificConfig and accepts AAC-LC only. */
aula_status aula_aac_parse_audio_specific_config(aula_bytes config,
                                                    aula_aac_config *out_config);
aula_status aula_aac_decoder_create(const aula_aac_config *config,
                                      aula_aac_decoder **out_decoder);
aula_status aula_aac_decoder_decode(aula_aac_decoder *decoder,
                                      aula_bytes access_unit,
                                      aula_mutable_bytes *out_pcm);
void aula_aac_decoder_destroy(aula_aac_decoder *decoder);
int aula_aac_native_pipeline_available(void);

#ifdef __cplusplus
}
#endif

#endif
