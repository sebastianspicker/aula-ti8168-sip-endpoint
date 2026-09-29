#ifndef AULA_SIPD_G711_H
#define AULA_SIPD_G711_H

#include "aula_sipd/config.h"
#include "aula_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_G711_SAMPLE_RATE 8000U
#define AULA_G711_PACKET_DURATION_MS 20U
#define AULA_G711_SAMPLES_PER_PACKET 160U

typedef enum aula_pcm_sample_format {
  AULA_PCM_S16LE = 0
} aula_pcm_sample_format;

typedef struct aula_pcm_format {
  aula_pcm_sample_format sample_format;
  uint32_t sample_rate;
  uint8_t channels;
} aula_pcm_format;

typedef struct aula_g711_packetizer {
  aula_g711_codec codec;
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t sequence_number;
  uint32_t rtp_timestamp;
} aula_g711_packetizer;

aula_status aula_g711_validate_pcm_format(const aula_pcm_format *format);
aula_status aula_g711_encode_pcmu(aula_bytes pcm_s16le,
                                    aula_mutable_bytes *output);
aula_status aula_g711_encode_pcma(aula_bytes pcm_s16le,
                                    aula_mutable_bytes *output);
aula_status aula_g711_decode_pcmu(aula_bytes encoded_audio,
                                    aula_mutable_bytes *output_pcm_s16le);
aula_status aula_g711_decode_pcma(aula_bytes encoded_audio,
                                    aula_mutable_bytes *output_pcm_s16le);
aula_status aula_g711_packetize_20ms(aula_g711_packetizer *packetizer,
                                       aula_bytes encoded_audio,
                                       aula_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
