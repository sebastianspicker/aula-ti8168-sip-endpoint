#include "aula_sipd/h264.h"

#include <stdlib.h>
#include <string.h>

struct aula_h264_depacketizer {
  uint8_t *access_unit;
  size_t length;
  size_t capacity;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t next_sequence;
  uint8_t fu_header;
  int active;
  int fu_active;
  int has_vcl;
};

static void reset_access_unit(aula_h264_depacketizer *state) {
  state->length = 0U;
  state->active = 0;
  state->fu_active = 0;
  state->has_vcl = 0;
}

static aula_status append_bytes(aula_h264_depacketizer *state,
                                  const uint8_t *data, size_t length) {
  if (length > state->capacity - state->length) return AULA_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(state->access_unit + state->length, data, length);
  state->length += length;
  return AULA_STATUS_OK;
}

static aula_status append_nal(aula_h264_depacketizer *state,
                                const uint8_t *data, size_t length) {
  static const uint8_t start_code[] = {0U, 0U, 0U, 1U};
  aula_status status;
  if (length == 0U || (data[0] & 0x80U) != 0U ||
      (data[0] & 0x1fU) == 0U || (data[0] & 0x1fU) > 23U || state->fu_active)
    return AULA_STATUS_INVALID_DATA;
  status = append_bytes(state, start_code, sizeof(start_code));
  if ((data[0] & 0x1fU) <= 5U) state->has_vcl = 1;
  return status == AULA_STATUS_OK ? append_bytes(state, data, length) : status;
}

static aula_status append_stap_a(aula_h264_depacketizer *state,
                                   aula_bytes payload) {
  size_t offset = 1U;
  if (payload.length < 4U) return AULA_STATUS_INVALID_DATA;
  while (offset < payload.length) {
    size_t length;
    aula_status status;
    if (payload.length - offset < 2U) return AULA_STATUS_INVALID_DATA;
    length = ((size_t)payload.data[offset] << 8U) | payload.data[offset + 1U];
    offset += 2U;
    if (length > payload.length - offset) return AULA_STATUS_INVALID_DATA;
    status = append_nal(state, payload.data + offset, length);
    if (status != AULA_STATUS_OK) return status;
    offset += length;
  }
  return AULA_STATUS_OK;
}

static aula_status append_fu_a(aula_h264_depacketizer *state,
                                 const aula_rtp_packet *packet) {
  aula_bytes payload = packet->payload;
  uint8_t header;
  int start;
  int end;
  aula_status status;
  if (payload.length < 3U || (payload.data[1] & 0x20U) != 0U)
    return AULA_STATUS_INVALID_DATA;
  header = (uint8_t)((payload.data[0] & 0xe0U) | (payload.data[1] & 0x1fU));
  start = (payload.data[1] & 0x80U) != 0U;
  end = (payload.data[1] & 0x40U) != 0U;
  if ((start && end) || (packet->header.marker && !end)) return AULA_STATUS_INVALID_DATA;
  if (start) {
    status = append_nal(state, &header, 1U);
    if (status != AULA_STATUS_OK) return status;
    state->fu_active = 1;
    state->fu_header = header;
  } else if (!state->fu_active || header != state->fu_header) {
    return AULA_STATUS_INVALID_DATA;
  }
  status = append_bytes(state, payload.data + 2U, payload.length - 2U);
  if (end) state->fu_active = 0;
  return status;
}

static aula_status append_payload(aula_h264_depacketizer *state,
                                    const aula_rtp_packet *packet) {
  uint8_t type = (uint8_t)(packet->payload.data[0] & 0x1fU);
  if ((packet->payload.data[0] & 0x80U) != 0U) return AULA_STATUS_INVALID_DATA;
  if (type >= 1U && type <= 23U)
    return append_nal(state, packet->payload.data, packet->payload.length);
  if (type == 24U) return append_stap_a(state, packet->payload);
  if (type == 28U) return append_fu_a(state, packet);
  return AULA_STATUS_UNSUPPORTED;
}

static aula_status finish_access_unit(aula_h264_depacketizer *state,
                                        aula_mutable_bytes *output, int *complete) {
  if (state->fu_active) return AULA_STATUS_INVALID_DATA;
  if (state->length > output->capacity) return AULA_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(output->data, state->access_unit, state->length);
  output->length = state->length;
  *complete = 1;
  reset_access_unit(state);
  return AULA_STATUS_OK;
}

static aula_status admit_packet(aula_h264_depacketizer *state,
                                  const aula_rtp_packet *packet) {
  if (state->active && state->ssrc != packet->header.ssrc) reset_access_unit(state);
  /* Parameter sets and leading SEI can precede the first timed picture.
   * Preserve this bounded prefix, but never carry slices across timestamps. */
  if (state->active && state->timestamp != packet->header.timestamp &&
      (state->has_vcl || state->fu_active)) reset_access_unit(state);
  if (state->active && state->next_sequence != packet->header.sequence_number)
    return AULA_STATUS_INVALID_DATA;
  state->active = 1;
  state->timestamp = packet->header.timestamp;
  state->ssrc = packet->header.ssrc;
  state->next_sequence = (uint16_t)(packet->header.sequence_number + 1U);
  return AULA_STATUS_OK;
}

aula_status aula_h264_depacketizer_create(uint32_t maximum_access_unit_bytes,
                                            aula_h264_depacketizer **out_depacketizer) {
  aula_h264_depacketizer *state;
  if (out_depacketizer == NULL || maximum_access_unit_bytes == 0U ||
      maximum_access_unit_bytes > AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES)
    return AULA_STATUS_INVALID_ARGUMENT;
  state = calloc(1U, sizeof(*state));
  if (state == NULL) return AULA_STATUS_INTERNAL_ERROR;
  state->access_unit = malloc(maximum_access_unit_bytes);
  if (state->access_unit == NULL) { free(state); return AULA_STATUS_INTERNAL_ERROR; }
  state->capacity = maximum_access_unit_bytes;
  *out_depacketizer = state;
  return AULA_STATUS_OK;
}

aula_status aula_h264_depacketizer_push(aula_h264_depacketizer *state,
                                          const aula_rtp_packet *packet,
                                          aula_mutable_bytes *output, int *complete) {
  aula_status status;
  if (state == NULL || packet == NULL || packet->payload.data == NULL ||
      packet->payload.length == 0U || output == NULL || output->data == NULL ||
      complete == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  output->length = 0U;
  *complete = 0;
  status = admit_packet(state, packet);
  if (status == AULA_STATUS_OK) status = append_payload(state, packet);
  if (status == AULA_STATUS_OK && packet->header.marker)
    status = finish_access_unit(state, output, complete);
  if (status != AULA_STATUS_OK) reset_access_unit(state);
  return status;
}

void aula_h264_depacketizer_destroy(aula_h264_depacketizer *state) {
  if (state == NULL) return;
  free(state->access_unit);
  free(state);
}
