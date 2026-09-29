#include "aec_internal.h"

#include <stdlib.h>
#include <string.h>

#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
#include <speex/speex_echo.h>
#endif

#define AULA_AEC_REFERENCE_SLOTS (AULA_AEC_MAX_DELAY_FRAMES + 1U)

struct aula_aec {
  aula_aec_config config;
  aula_aec_status status;
  int16_t reference[AULA_AEC_REFERENCE_SLOTS][AULA_AEC_FRAME_SAMPLES];
  unsigned int head;
  unsigned int tail;
  unsigned int count;
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  SpeexEchoState *native;
#endif
};

static int frame_is_valid(aula_bytes frame) {
  return frame.data != NULL && frame.length == AULA_AEC_FRAME_BYTES;
}

static int config_is_valid(const aula_aec_config *config, aula_aec *const *out_aec) {
  return config != NULL && out_aec != NULL && *out_aec == NULL &&
      (config->enabled == 0 || config->enabled == 1) &&
      config->reference_delay_frames <= AULA_AEC_MAX_DELAY_FRAMES &&
      (config->delay_calibrated == 0 || config->delay_calibrated == 1);
}

static void aec_copy(aula_bytes input, aula_mutable_bytes *output) {
  (void)memcpy(output->data, input.data, input.length);
  output->length = input.length;
}

static int16_t aec_read_s16le(const uint8_t *data) {
  uint16_t value = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
  return (int16_t)value;
}

static void aec_decode_s16le(aula_bytes input, int16_t *samples) {
  size_t index;
  for (index = 0U; index < AULA_AEC_FRAME_SAMPLES; index++) {
    samples[index] = aec_read_s16le(input.data + index * 2U);
  }
}

static void aec_write_s16le(int16_t value, uint8_t *data) {
  uint16_t raw = (uint16_t)value;
  data[0] = (uint8_t)(raw & 0xffU);
  data[1] = (uint8_t)(raw >> 8U);
}

static void aec_encode_s16le(const int16_t *samples, aula_mutable_bytes *output) {
  size_t index;
  for (index = 0U; index < AULA_AEC_FRAME_SAMPLES; index++) {
    aec_write_s16le(samples[index], output->data + index * 2U);
  }
  output->length = AULA_AEC_FRAME_BYTES;
}

int aula_aec_native_available(void) {
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  return 1;
#else
  return 0;
#endif
}

aula_status aula_aec_create(const aula_aec_config *config, aula_aec **out_aec) {
  aula_aec *aec;
  if (!config_is_valid(config, out_aec)) return AULA_STATUS_INVALID_ARGUMENT;
  aec = calloc(1U, sizeof(*aec));
  if (aec == NULL) return AULA_STATUS_INTERNAL_ERROR;
  aec->config = *config;
  aec->status.available = aula_aec_native_available();
  aec->status.delay_state = config->enabled == 0 ? AULA_AEC_DELAY_INACTIVE :
      (aec->status.available == 0 ? AULA_AEC_DELAY_INACTIVE :
       (config->delay_calibrated != 0 ? AULA_AEC_DELAY_CALIBRATED : AULA_AEC_DELAY_UNCALIBRATED));
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  if (config->enabled != 0) {
    int sample_rate = AULA_AEC_SAMPLE_RATE;
    aec->native = speex_echo_state_init(AULA_AEC_FRAME_SAMPLES,
                                        AULA_AEC_FRAME_SAMPLES * 8U);
    if (aec->native == NULL || speex_echo_ctl(aec->native, SPEEX_ECHO_SET_SAMPLING_RATE,
                                               &sample_rate) != 0) {
      if (aec->native != NULL) speex_echo_state_destroy(aec->native);
      free(aec);
      return AULA_STATUS_INTERNAL_ERROR;
    }
  }
#endif
  *out_aec = aec;
  return AULA_STATUS_OK;
}

aula_status aula_aec_push_reference(aula_aec *aec, aula_bytes frame) {
  unsigned int slot;
  if (aec == NULL || !frame_is_valid(frame)) return AULA_STATUS_INVALID_ARGUMENT;
  if (aec->config.enabled == 0 || aec->status.available == 0) return AULA_STATUS_AGAIN;
  if (aec->count == AULA_AEC_REFERENCE_SLOTS) {
    aula_aec_reset(aec);
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  slot = aec->head % AULA_AEC_REFERENCE_SLOTS;
  aec_decode_s16le(frame, aec->reference[slot]);
  aec->head = (aec->head + 1U) % AULA_AEC_REFERENCE_SLOTS;
  if (aec->count < AULA_AEC_REFERENCE_SLOTS) aec->count++;
  return AULA_STATUS_OK;
}

static int aec_reference_slot(const aula_aec *aec, unsigned int *out_slot) {
  if (aec->count <= aec->config.reference_delay_frames) return 0;
  *out_slot = aec->tail;
  return 1;
}

#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
static void aec_consume_reference(aula_aec *aec) {
  (void)memset(aec->reference[aec->tail], 0, sizeof(aec->reference[aec->tail]));
  aec->tail = (aec->tail + 1U) % AULA_AEC_REFERENCE_SLOTS;
  aec->count--;
}
#endif

aula_status aula_aec_copy_delayed_reference(const aula_aec *aec,
                                              aula_mutable_bytes *out_frame) {
  unsigned int slot;
  if (aec == NULL || out_frame == NULL || out_frame->data == NULL ||
      out_frame->capacity < AULA_AEC_FRAME_BYTES) return AULA_STATUS_INVALID_ARGUMENT;
  if (!aec_reference_slot(aec, &slot)) return AULA_STATUS_AGAIN;
  aec_encode_s16le(aec->reference[slot], out_frame);
  return AULA_STATUS_OK;
}

aula_status aula_aec_process_near(aula_aec *aec, aula_bytes near,
                                    aula_mutable_bytes *output) {
  unsigned int slot;
  if (aec == NULL || output == NULL || output->data == NULL || !frame_is_valid(near) ||
      output->capacity < near.length) return AULA_STATUS_INVALID_ARGUMENT;
  if (aec->config.enabled == 0 || aec->status.available == 0 || !aec_reference_slot(aec, &slot)) {
    aec_copy(near, output);
    aec->status.active = 0;
    aec->status.bypassed_frames++;
    if (aec->config.enabled != 0 && aec->status.available != 0) aec->status.reference_underflows++;
    return AULA_STATUS_AGAIN;
  }
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  int16_t near_samples[AULA_AEC_FRAME_SAMPLES];
  int16_t output_samples[AULA_AEC_FRAME_SAMPLES];
  aec_decode_s16le(near, near_samples);
  speex_echo_cancellation(aec->native, near_samples, aec->reference[slot], output_samples);
  aec_encode_s16le(output_samples, output);
  aec_consume_reference(aec);
  aec->status.active = 1;
  aec->status.processed_frames++;
  return AULA_STATUS_OK;
#else
  (void)slot;
  aec_copy(near, output);
  aec->status.active = 0;
  aec->status.bypassed_frames++;
  return AULA_STATUS_AGAIN;
#endif
}

aula_status aula_aec_get_status(const aula_aec *aec, aula_aec_status *out_status) {
  if (aec == NULL || out_status == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_status = aec->status;
  return AULA_STATUS_OK;
}

void aula_aec_reset(aula_aec *aec) {
  if (aec == NULL) return;
  (void)memset(aec->reference, 0, sizeof(aec->reference));
  aec->head = 0U;
  aec->tail = 0U;
  aec->count = 0U;
  aec->status.active = 0;
  aec->status.resets++;
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  if (aec->native != NULL) speex_echo_state_reset(aec->native);
#endif
}

void aula_aec_destroy(aula_aec *aec) {
  if (aec == NULL) return;
#if defined(AULA_SIPD_HAVE_NATIVE_AEC) && AULA_SIPD_HAVE_NATIVE_AEC
  if (aec->native != NULL) speex_echo_state_destroy(aec->native);
#endif
  (void)memset(aec, 0, sizeof(*aec));
  free(aec);
}
