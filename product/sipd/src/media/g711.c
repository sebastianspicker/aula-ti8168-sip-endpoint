#include "ls200_sipd/g711.h"

#include <limits.h>

static uint8_t ls200_g711_search_exponent(int32_t value) {
  uint8_t exponent = 0U;
  int32_t threshold = 0x100;
  while (exponent < 7U && value >= threshold) {
    exponent++;
    threshold <<= 1;
  }
  return exponent;
}

static uint8_t ls200_g711_pcmu_sample(int16_t input) {
  int32_t sample = input;
  uint8_t sign = sample < 0 ? 0x80U : 0U;
  uint8_t exponent;
  uint8_t mantissa;
  if (sample < 0) {
    sample = -sample;
  }
  if (sample > 32635) {
    sample = 32635;
  }
  sample += 0x84;
  exponent = ls200_g711_search_exponent(sample);
  mantissa = (uint8_t)((sample >> (exponent + 3U)) & 0x0f);
  return (uint8_t)~(sign | (uint8_t)(exponent << 4U) | mantissa);
}

static uint8_t ls200_g711_pcma_sample(int16_t input) {
  static const int32_t segment_ends[] = {
    0x1f, 0x3f, 0x7f, 0xff, 0x1ff, 0x3ff, 0x7ff, 0xfff
  };
  int32_t sample = (int32_t)input >> 3;
  uint8_t mask;
  uint8_t segment;
  uint8_t value;
  if (sample >= 0) {
    mask = 0xd5U;
  } else {
    mask = 0x55U;
    sample = -sample - 1;
  }
  for (segment = 0U; segment < 8U && sample > segment_ends[segment]; ++segment) {
  }
  if (segment >= 8U) return (uint8_t)(0x7fU ^ mask);
  value = (uint8_t)(segment << 4U);
  value = (uint8_t)(value |
      (uint8_t)(((segment < 2U ? sample >> 1 : sample >> segment)) & 0x0f));
  return (uint8_t)(value ^ mask);
}

static ls200_status ls200_g711_encode(ls200_bytes pcm_s16le,
                                      ls200_mutable_bytes *output, int use_pcma) {
  size_t index;
  size_t samples;
  if (pcm_s16le.data == NULL || output == NULL || output->data == NULL ||
      (pcm_s16le.length & 1U) != 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  samples = pcm_s16le.length / 2U;
  if (samples > output->capacity) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  for (index = 0U; index < samples; index++) {
    uint16_t raw = (uint16_t)((uint16_t)pcm_s16le.data[index * 2U] |
        (uint16_t)((uint16_t)pcm_s16le.data[index * 2U + 1U] << 8U));
    int16_t sample = (int16_t)raw;
    output->data[index] = use_pcma != 0 ? ls200_g711_pcma_sample(sample) :
                                          ls200_g711_pcmu_sample(sample);
  }
  output->length = samples;
  return LS200_STATUS_OK;
}

ls200_status ls200_g711_validate_pcm_format(const ls200_pcm_format *format) {
  if (format == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (format->sample_format != LS200_PCM_S16LE ||
      format->sample_rate != LS200_G711_SAMPLE_RATE || format->channels != 1U) {
    return LS200_STATUS_UNSUPPORTED;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_g711_encode_pcmu(ls200_bytes pcm_s16le,
                                    ls200_mutable_bytes *output) {
  return ls200_g711_encode(pcm_s16le, output, 0);
}

ls200_status ls200_g711_encode_pcma(ls200_bytes pcm_s16le,
                                    ls200_mutable_bytes *output) {
  return ls200_g711_encode(pcm_s16le, output, 1);
}

static int16_t ls200_g711_decode_pcmu_sample(uint8_t encoded) {
  int32_t value;
  uint8_t sample = (uint8_t)~encoded;
  value = ((int32_t)(sample & 0x0fU) << 3) + 0x84;
  value <<= (sample >> 4U) & 0x07U;
  value -= 0x84;
  return (int16_t)((sample & 0x80U) != 0U ? -value : value);
}

static int16_t ls200_g711_decode_pcma_sample(uint8_t encoded) {
  int32_t value;
  uint8_t sample = (uint8_t)(encoded ^ 0x55U);
  uint8_t segment = (uint8_t)((sample & 0x70U) >> 4U);
  value = (int32_t)(sample & 0x0fU) << 4U;
  if (segment == 0U) value += 8;
  else if (segment == 1U) value += 0x108;
  else { value += 0x108; value <<= segment - 1U; }
  return (int16_t)((sample & 0x80U) != 0U ? value : -value);
}

static ls200_status ls200_g711_decode(ls200_bytes encoded_audio,
                                      ls200_mutable_bytes *output, int pcma) {
  size_t index;
  if (encoded_audio.data == NULL || output == NULL || output->data == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (encoded_audio.length > output->capacity / 2U)
    return LS200_STATUS_LIMIT_EXCEEDED;
  for (index = 0U; index < encoded_audio.length; ++index) {
    uint16_t sample = (uint16_t)(pcma != 0
        ? ls200_g711_decode_pcma_sample(encoded_audio.data[index])
        : ls200_g711_decode_pcmu_sample(encoded_audio.data[index]));
    output->data[index * 2U] = (uint8_t)sample;
    output->data[index * 2U + 1U] = (uint8_t)(sample >> 8U);
  }
  output->length = encoded_audio.length * 2U;
  return LS200_STATUS_OK;
}

ls200_status ls200_g711_decode_pcmu(ls200_bytes encoded_audio,
                                    ls200_mutable_bytes *output_pcm_s16le) {
  return ls200_g711_decode(encoded_audio, output_pcm_s16le, 0);
}

ls200_status ls200_g711_decode_pcma(ls200_bytes encoded_audio,
                                    ls200_mutable_bytes *output_pcm_s16le) {
  return ls200_g711_decode(encoded_audio, output_pcm_s16le, 1);
}

ls200_status ls200_g711_packetize_20ms(ls200_g711_packetizer *packetizer,
                                       ls200_bytes encoded_audio,
                                       ls200_mutable_bytes *output) {
  size_t index;
  if (packetizer == NULL || encoded_audio.data == NULL || output == NULL ||
      output->data == NULL || encoded_audio.length != LS200_G711_SAMPLES_PER_PACKET) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (output->capacity < encoded_audio.length + 12U) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  output->data[0] = 0x80U;
  output->data[1] = packetizer->payload_type;
  output->data[2] = (uint8_t)(packetizer->sequence_number >> 8U);
  output->data[3] = (uint8_t)packetizer->sequence_number;
  output->data[4] = (uint8_t)(packetizer->rtp_timestamp >> 24U);
  output->data[5] = (uint8_t)(packetizer->rtp_timestamp >> 16U);
  output->data[6] = (uint8_t)(packetizer->rtp_timestamp >> 8U);
  output->data[7] = (uint8_t)packetizer->rtp_timestamp;
  output->data[8] = (uint8_t)(packetizer->ssrc >> 24U);
  output->data[9] = (uint8_t)(packetizer->ssrc >> 16U);
  output->data[10] = (uint8_t)(packetizer->ssrc >> 8U);
  output->data[11] = (uint8_t)packetizer->ssrc;
  for (index = 0U; index < encoded_audio.length; index++) {
    output->data[12U + index] = encoded_audio.data[index];
  }
  output->length = encoded_audio.length + 12U;
  packetizer->sequence_number = (uint16_t)(packetizer->sequence_number + 1U);
  packetizer->rtp_timestamp += LS200_G711_SAMPLES_PER_PACKET;
  return LS200_STATUS_OK;
}

ls200_status ls200_media_validate_frame(const ls200_media_frame *frame) {
  if (frame == NULL || frame->data.data == NULL || frame->data.length == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (frame->kind == LS200_MEDIA_VIDEO_H264_ANNEX_B) {
    if (frame->data.length > LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES ||
        frame->sample_rate != 0U || frame->channels != 0U) {
      return LS200_STATUS_LIMIT_EXCEEDED;
    }
    return LS200_STATUS_OK;
  }
  if (frame->kind == LS200_MEDIA_AUDIO_PCM_S16LE) {
    ls200_pcm_format format;
    if (frame->data.length > LS200_SIPD_MAX_AUDIO_FRAME_BYTES ||
        (frame->data.length & 1U) != 0U) {
      return LS200_STATUS_LIMIT_EXCEEDED;
    }
    format.sample_format = LS200_PCM_S16LE;
    format.sample_rate = frame->sample_rate;
    format.channels = frame->channels;
    return ls200_g711_validate_pcm_format(&format);
  }
  return LS200_STATUS_UNSUPPORTED;
}

ls200_status ls200_media_g711_encode(ls200_media_kind input_kind,
                                     ls200_bytes pcm_s16le, uint32_t sample_rate,
                                     uint8_t channels, int use_pcma,
                                     ls200_mutable_bytes *output) {
  ls200_pcm_format format;
  ls200_status status;
  if (input_kind != LS200_MEDIA_AUDIO_PCM_S16LE || output == NULL ||
      output->data == NULL || (pcm_s16le.length & 1U) != 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  format.sample_format = LS200_PCM_S16LE;
  format.sample_rate = sample_rate;
  format.channels = channels;
  status = ls200_g711_validate_pcm_format(&format);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  return use_pcma != 0 ? ls200_g711_encode_pcma(pcm_s16le, output) :
                         ls200_g711_encode_pcmu(pcm_s16le, output);
}
