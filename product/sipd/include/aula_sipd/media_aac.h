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
/* Decodes one MPEG4-GENERIC AU to signed 16-bit 8 kHz mono.  It is available
 * only with explicitly provided FAAD2 2.11.2 and SpeexDSP 1.2.1 builds.  The
 * stateful decoder API above must be used for a stream; this helper is for a
 * single independently decodable access unit. */
aula_status aula_aac_decode_to_g711_pcm(aula_bytes access_unit,
                                          const aula_aac_config *config,
                                          aula_mutable_bytes *out_pcm);
int aula_aac_native_pipeline_available(void);

#ifdef __cplusplus
}
#endif

#endif
