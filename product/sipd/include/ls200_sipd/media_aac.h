#ifndef LS200_SIPD_MEDIA_AAC_H
#define LS200_SIPD_MEDIA_AAC_H

#include "ls200_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_aac_config {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t audio_object_type;
} ls200_aac_config;

typedef struct ls200_aac_decoder ls200_aac_decoder;

/* Parses MPEG-4 AudioSpecificConfig and accepts AAC-LC only. */
ls200_status ls200_aac_parse_audio_specific_config(ls200_bytes config,
                                                    ls200_aac_config *out_config);
ls200_status ls200_aac_decoder_create(const ls200_aac_config *config,
                                      ls200_aac_decoder **out_decoder);
ls200_status ls200_aac_decoder_decode(ls200_aac_decoder *decoder,
                                      ls200_bytes access_unit,
                                      ls200_mutable_bytes *out_pcm);
void ls200_aac_decoder_destroy(ls200_aac_decoder *decoder);
/* Decodes one MPEG4-GENERIC AU to signed 16-bit 8 kHz mono.  It is available
 * only with explicitly provided FAAD2 2.11.2 and SpeexDSP 1.2.1 builds.  The
 * stateful decoder API above must be used for a stream; this helper is for a
 * single independently decodable access unit. */
ls200_status ls200_aac_decode_to_g711_pcm(ls200_bytes access_unit,
                                          const ls200_aac_config *config,
                                          ls200_mutable_bytes *out_pcm);
int ls200_aac_native_pipeline_available(void);

#ifdef __cplusplus
}
#endif

#endif
