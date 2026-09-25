#include "ls200_sipd/media_aac.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
#include <neaacdec.h>
#include <speex/speex_resampler.h>
#endif

#define LS200_AAC_MAX_ACCESS_UNIT_BYTES 4096U
#define LS200_AAC_OUTPUT_SAMPLE_RATE 8000U

#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
struct ls200_aac_decoder {
  NeAACDecHandle faad;
  SpeexResamplerState *resampler;
  ls200_aac_config config;
  uint32_t decoded_sample_rate;
};
#endif

static const uint32_t LS200_AAC_SAMPLE_RATES[] = {
  96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
  22050U, 16000U, 12000U, 11025U, 8000U, 7350U
};

static ls200_status aac_read_bits(ls200_bytes input, size_t *bit_offset,
                                  unsigned int bits, uint32_t *out_value) {
  uint32_t value = 0U;
  unsigned int index;
  if (input.data == NULL || bit_offset == NULL || out_value == NULL ||
      bits == 0U || bits > 16U || *bit_offset > input.length * 8U ||
      bits > input.length * 8U - *bit_offset) {
    return LS200_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < bits; ++index) {
    size_t offset = *bit_offset + index;
    value = (value << 1U) |
        (((uint32_t)input.data[offset / 8U] >> (7U - offset % 8U)) & 1U);
  }
  *bit_offset += bits;
  *out_value = value;
  return LS200_STATUS_OK;
}

static ls200_status aac_validate_gaspecific(ls200_bytes config, size_t *offset) {
  uint32_t bit;
  unsigned int index;
  for (index = 0U; index < 3U; ++index) {
    if (aac_read_bits(config, offset, 1U, &bit) != LS200_STATUS_OK) {
      return LS200_STATUS_INVALID_DATA;
    }
    if (bit != 0U) return LS200_STATUS_UNSUPPORTED;
  }
  return *offset == config.length * 8U ? LS200_STATUS_OK : LS200_STATUS_INVALID_DATA;
}

ls200_status ls200_aac_parse_audio_specific_config(ls200_bytes config,
                                                    ls200_aac_config *out_config) {
  size_t offset = 0U;
  uint32_t object_type;
  uint32_t frequency_index;
  uint32_t channels;
  ls200_status status;
  if (out_config == NULL || config.data == NULL || config.length != 2U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(out_config, 0, sizeof(*out_config));
  status = aac_read_bits(config, &offset, 5U, &object_type);
  if (status != LS200_STATUS_OK || object_type == 31U) {
    return LS200_STATUS_UNSUPPORTED;
  }
  status = aac_read_bits(config, &offset, 4U, &frequency_index);
  if (status != LS200_STATUS_OK ||
      frequency_index >= sizeof(LS200_AAC_SAMPLE_RATES) /
          sizeof(LS200_AAC_SAMPLE_RATES[0])) {
    return LS200_STATUS_INVALID_DATA;
  }
  status = aac_read_bits(config, &offset, 4U, &channels);
  if (status != LS200_STATUS_OK || object_type != 2U || channels == 0U ||
      channels > 2U) {
    return LS200_STATUS_UNSUPPORTED;
  }
  /* AAC-LC's supported GASpecificConfig has all three flag bits clear. */
  status = aac_validate_gaspecific(config, &offset);
  if (status != LS200_STATUS_OK) return status;
  out_config->audio_object_type = (uint8_t)object_type;
  out_config->sample_rate = LS200_AAC_SAMPLE_RATES[frequency_index];
  out_config->channels = (uint8_t)channels;
  return LS200_STATUS_OK;
}

int ls200_aac_native_pipeline_available(void) {
#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
  return 1;
#else
  return 0;
#endif
}

#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
static int aac_frequency_index(uint32_t sample_rate, uint8_t *out_index) {
  size_t index;
  if (out_index == NULL) return 0;
  for (index = 0U; index < sizeof(LS200_AAC_SAMPLE_RATES) /
                            sizeof(LS200_AAC_SAMPLE_RATES[0]); ++index) {
    if (LS200_AAC_SAMPLE_RATES[index] == sample_rate) {
      *out_index = (uint8_t)index;
      return 1;
    }
  }
  return 0;
}

static ls200_status aac_build_asc(const ls200_aac_config *config,
                                  uint8_t out_asc[2]) {
  uint8_t frequency_index;
  if (config == NULL || out_asc == NULL || config->audio_object_type != 2U ||
      config->channels == 0U || config->channels > 2U ||
      !aac_frequency_index(config->sample_rate, &frequency_index)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  out_asc[0] = (uint8_t)((2U << 3U) | (frequency_index >> 1U));
  out_asc[1] = (uint8_t)(((uint32_t)(frequency_index & 1U) << 7U) |
                         ((uint32_t)config->channels << 3U));
  return LS200_STATUS_OK;
}

static int16_t aac_downmix_sample(const int16_t *input, uint8_t channels) {
  int32_t total = 0;
  uint8_t channel;
  for (channel = 0U; channel < channels; ++channel) total += input[channel];
  return (int16_t)(total / (int32_t)channels);
}

#endif

#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
static int aac_decoder_init_valid(const ls200_aac_config *config,
                                  unsigned long decoded_rate,
                                  unsigned char decoded_channels) {
  if (decoded_rate == 0UL || decoded_rate > (unsigned long)UINT32_MAX) return 0;
  if (decoded_rate != (unsigned long)config->sample_rate) {
    if (config->sample_rate > UINT32_MAX / 2U) return 0;
    if (decoded_rate != (unsigned long)config->sample_rate * 2UL) return 0;
  }
  if (decoded_channels == 0U || decoded_channels > 2U) return 0;
  if (config->channels == 2U && decoded_channels != 2U) return 0;
  return 1;
}

static ls200_status aac_open_decoder(const ls200_aac_config *config,
                                     ls200_aac_decoder *decoder) {
  uint8_t asc[2];
  NeAACDecConfigurationPtr decoder_config;
  unsigned long decoded_rate = 0UL;
  unsigned char decoded_channels = 0U;
  NeAACDecHandle faad;
  if (decoder == NULL || aac_build_asc(config, asc) != LS200_STATUS_OK) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  faad = NeAACDecOpen();
  if (faad == NULL) return LS200_STATUS_INTERNAL_ERROR;
  decoder_config = NeAACDecGetCurrentConfiguration(faad);
  if (decoder_config == NULL) {
    NeAACDecClose(faad);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  decoder_config->outputFormat = FAAD_FMT_16BIT;
  decoder_config->downMatrix = 0U;
  decoder_config->defSampleRate = (unsigned long)config->sample_rate;
  decoder_config->dontUpSampleImplicitSBR = 1U;
  if (NeAACDecSetConfiguration(faad, decoder_config) == 0U ||
      NeAACDecInit2(faad, asc, sizeof(asc), &decoded_rate, &decoded_channels) < 0L ||
      !aac_decoder_init_valid(config, decoded_rate, decoded_channels)) {
    NeAACDecClose(faad);
    return LS200_STATUS_INVALID_DATA;
  }
  decoder->faad = faad;
  return LS200_STATUS_OK;
}

static int aac_decoded_frame_valid(const ls200_aac_decoder *decoder,
                                   ls200_bytes access_unit,
                                   const NeAACDecFrameInfo *frame_info,
                                   const void *decoded) {
  if (decoded == NULL || frame_info->error != 0U) return 0;
  if (frame_info->bytesconsumed != (unsigned long)access_unit.length) return 0;
  if (frame_info->object_type != 2U || frame_info->sbr != 0U) return 0;
  if (frame_info->ps != 0U) return 0;
  if (frame_info->samplerate != decoder->config.sample_rate) return 0;
  if (frame_info->channels == 0U || frame_info->channels > 2U) return 0;
  if (decoder->config.channels == 2U && frame_info->channels != 2U) return 0;
  if (frame_info->samples % frame_info->channels != 0UL) return 0;
  return 1;
}

static ls200_status aac_decode_downmix(ls200_aac_decoder *decoder,
                                       ls200_bytes access_unit,
                                       int16_t *mono, size_t capacity,
                                       size_t *out_frames,
                                       uint32_t *out_sample_rate) {
  NeAACDecFrameInfo frame_info;
  void *decoded;
  size_t frame_count;
  size_t index;
  if (decoder == NULL || decoder->faad == NULL || mono == NULL ||
      out_frames == NULL || out_sample_rate == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(&frame_info, 0, sizeof(frame_info));
  decoded = NeAACDecDecode(decoder->faad, &frame_info,
                           (unsigned char *)access_unit.data,
                           (unsigned long)access_unit.length);
  if (!aac_decoded_frame_valid(decoder, access_unit, &frame_info, decoded)) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (frame_info.samples == 0UL) return LS200_STATUS_AGAIN;
  frame_count = (size_t)(frame_info.samples / frame_info.channels);
  if (frame_count > capacity || frame_count > (size_t)UINT_MAX) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  for (index = 0U; index < frame_count; ++index) {
    mono[index] = aac_downmix_sample(
        (const int16_t *)decoded + index * frame_info.channels,
                                     frame_info.channels);
  }
  *out_frames = frame_count;
  *out_sample_rate = (uint32_t)frame_info.samplerate;
  return LS200_STATUS_OK;
}

static ls200_status aac_resample_mono(ls200_aac_decoder *decoder,
                                      const int16_t *mono, size_t input_count,
                                      uint32_t source_rate,
                                      ls200_mutable_bytes *out_pcm) {
  int16_t output[LS200_SIPD_MAX_AUDIO_FRAME_BYTES / sizeof(int16_t)];
  SpeexResamplerState *resampler;
  spx_uint32_t input_frames;
  spx_uint32_t output_frames;
  size_t output_capacity;
  int speex_error = RESAMPLER_ERR_SUCCESS;
  if (input_count > (size_t)UINT_MAX ||
      out_pcm->capacity / sizeof(int16_t) > (size_t)UINT_MAX) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  input_frames = (spx_uint32_t)input_count;
  output_capacity = out_pcm->capacity / sizeof(int16_t);
  if (output_capacity > sizeof(output) / sizeof(output[0])) {
    output_capacity = sizeof(output) / sizeof(output[0]);
  }
  output_frames = (spx_uint32_t)output_capacity;
  if (output_frames == 0U) return LS200_STATUS_LIMIT_EXCEEDED;
  if (decoder->resampler == NULL) {
    decoder->resampler = speex_resampler_init(
        1U, source_rate, LS200_AAC_OUTPUT_SAMPLE_RATE,
        SPEEX_RESAMPLER_QUALITY_DEFAULT, &speex_error);
    decoder->decoded_sample_rate = source_rate;
    if (decoder->resampler == NULL || speex_error != RESAMPLER_ERR_SUCCESS) {
      if (decoder->resampler != NULL) {
        speex_resampler_destroy(decoder->resampler);
        decoder->resampler = NULL;
      }
      return LS200_STATUS_INTERNAL_ERROR;
    }
  } else if (decoder->decoded_sample_rate != source_rate) {
    return LS200_STATUS_INVALID_DATA;
  }
  resampler = decoder->resampler;
  speex_error = speex_resampler_process_int(resampler, 0U, mono, &input_frames,
                                            output, &output_frames);
  if (speex_error != RESAMPLER_ERR_SUCCESS || input_frames != (spx_uint32_t)input_count) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  if (output_frames == 0U) return LS200_STATUS_AGAIN;
  out_pcm->length = (size_t)output_frames * sizeof(int16_t);
  if (out_pcm->length != 0U) {
    (void)memcpy(out_pcm->data, output, out_pcm->length);
  }
  return LS200_STATUS_OK;
}

static ls200_status aac_native_decode(ls200_aac_decoder *decoder,
                                      ls200_bytes access_unit,
                                      ls200_mutable_bytes *out_pcm) {
  int16_t mono[LS200_SIPD_MAX_AUDIO_FRAME_BYTES / 2U];
  size_t frames = 0U;
  uint32_t source_rate = 0U;
  ls200_status status = aac_decode_downmix(
      decoder, access_unit, mono, sizeof(mono) / sizeof(mono[0]), &frames,
      &source_rate);
  if (status == LS200_STATUS_OK) {
    status = aac_resample_mono(decoder, mono, frames, source_rate, out_pcm);
  }
  return status;
}
#endif

ls200_status ls200_aac_decoder_create(const ls200_aac_config *config,
                                      ls200_aac_decoder **out_decoder) {
  if (config == NULL || out_decoder == NULL || config->audio_object_type != 2U ||
      config->sample_rate == 0U || config->channels == 0U ||
      config->channels > 2U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_decoder = NULL;
#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
  {
    ls200_aac_decoder *decoder = calloc(1U, sizeof(*decoder));
    ls200_status status;
    if (decoder == NULL) return LS200_STATUS_INTERNAL_ERROR;
    decoder->config = *config;
    status = aac_open_decoder(config, decoder);
    if (status != LS200_STATUS_OK) {
      free(decoder);
      return status;
    }
    *out_decoder = decoder;
    return LS200_STATUS_OK;
  }
#else
  return LS200_STATUS_UNSUPPORTED;
#endif
}

ls200_status ls200_aac_decoder_decode(ls200_aac_decoder *decoder,
                                      ls200_bytes access_unit,
                                      ls200_mutable_bytes *out_pcm) {
  if (decoder == NULL || access_unit.data == NULL || access_unit.length == 0U ||
      access_unit.length > LS200_AAC_MAX_ACCESS_UNIT_BYTES || out_pcm == NULL ||
      (out_pcm->data == NULL && out_pcm->capacity != 0U)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  out_pcm->length = 0U;
#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
  return aac_native_decode(decoder, access_unit, out_pcm);
#else
  return LS200_STATUS_UNSUPPORTED;
#endif
}

void ls200_aac_decoder_destroy(ls200_aac_decoder *decoder) {
#if defined(LS200_SIPD_HAVE_NATIVE_AAC) && LS200_SIPD_HAVE_NATIVE_AAC
  if (decoder == NULL) return;
  if (decoder->resampler != NULL) speex_resampler_destroy(decoder->resampler);
  if (decoder->faad != NULL) NeAACDecClose(decoder->faad);
  free(decoder);
#else
  (void)decoder;
#endif
}

ls200_status ls200_aac_decode_to_g711_pcm(ls200_bytes access_unit,
                                          const ls200_aac_config *config,
                                          ls200_mutable_bytes *out_pcm) {
  if (access_unit.data == NULL || access_unit.length == 0U ||
      access_unit.length > LS200_AAC_MAX_ACCESS_UNIT_BYTES || config == NULL ||
      out_pcm == NULL || (out_pcm->data == NULL && out_pcm->capacity != 0U) ||
      config->audio_object_type != 2U || config->channels == 0U ||
      config->channels > 2U || config->sample_rate == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  out_pcm->length = 0U;
  {
    ls200_aac_decoder *decoder = NULL;
    ls200_status status = ls200_aac_decoder_create(config, &decoder);
    if (status == LS200_STATUS_OK) {
      status = ls200_aac_decoder_decode(decoder, access_unit, out_pcm);
    }
    ls200_aac_decoder_destroy(decoder);
    return status;
  }
}
