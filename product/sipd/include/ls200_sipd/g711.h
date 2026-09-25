#ifndef LS200_SIPD_G711_H
#define LS200_SIPD_G711_H

#include "ls200_sipd/config.h"
#include "ls200_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_G711_SAMPLE_RATE 8000U
#define LS200_G711_PACKET_DURATION_MS 20U
#define LS200_G711_SAMPLES_PER_PACKET 160U

typedef enum ls200_pcm_sample_format {
  LS200_PCM_S16LE = 0
} ls200_pcm_sample_format;

typedef struct ls200_pcm_format {
  ls200_pcm_sample_format sample_format;
  uint32_t sample_rate;
  uint8_t channels;
} ls200_pcm_format;

typedef struct ls200_g711_packetizer {
  ls200_g711_codec codec;
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t sequence_number;
  uint32_t rtp_timestamp;
} ls200_g711_packetizer;

ls200_status ls200_g711_validate_pcm_format(const ls200_pcm_format *format);
ls200_status ls200_g711_encode_pcmu(ls200_bytes pcm_s16le,
                                    ls200_mutable_bytes *output);
ls200_status ls200_g711_encode_pcma(ls200_bytes pcm_s16le,
                                    ls200_mutable_bytes *output);
ls200_status ls200_g711_decode_pcmu(ls200_bytes encoded_audio,
                                    ls200_mutable_bytes *output_pcm_s16le);
ls200_status ls200_g711_decode_pcma(ls200_bytes encoded_audio,
                                    ls200_mutable_bytes *output_pcm_s16le);
ls200_status ls200_g711_packetize_20ms(ls200_g711_packetizer *packetizer,
                                       ls200_bytes encoded_audio,
                                       ls200_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
