#include "wire.h"

#include <string.h>

#define AULA_RTP_FIXED_HEADER_BYTES 12U

static uint16_t aula_rtp_wire_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t aula_rtp_wire_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void aula_rtp_wire_write_u16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

static void aula_rtp_wire_write_u32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

aula_status aula_rtp_wire_parse(aula_bytes input, aula_rtp_packet *out_packet) {
  const uint8_t *data;
  size_t header_bytes;
  size_t extension_bytes;
  size_t payload_bytes;
  uint8_t csrc_count;
  uint8_t padding_bytes = 0U;
  if (out_packet == NULL || (input.length != 0U && input.data == NULL)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(out_packet, 0, sizeof(*out_packet));
  if (input.length < AULA_RTP_FIXED_HEADER_BYTES) return AULA_STATUS_INVALID_DATA;
  if (input.length > AULA_SIPD_MAX_RTP_PACKET_BYTES) return AULA_STATUS_LIMIT_EXCEEDED;
  data = input.data;
  if ((data[0] >> 6) != 2U) return AULA_STATUS_INVALID_DATA;
  csrc_count = (uint8_t)(data[0] & 0x0fU);
  header_bytes = AULA_RTP_FIXED_HEADER_BYTES + ((size_t)csrc_count * 4U);
  if (header_bytes > input.length) return AULA_STATUS_INVALID_DATA;
  if ((data[0] & 0x10U) != 0U) {
    if (header_bytes > input.length - 4U) return AULA_STATUS_INVALID_DATA;
    extension_bytes = 4U + ((size_t)aula_rtp_wire_read_u16(data + header_bytes + 2U) * 4U);
    if (extension_bytes > input.length - header_bytes) return AULA_STATUS_INVALID_DATA;
    header_bytes += extension_bytes;
  }
  if ((data[0] & 0x20U) != 0U) {
    padding_bytes = data[input.length - 1U];
    if (padding_bytes == 0U || (size_t)padding_bytes > input.length - header_bytes) {
      return AULA_STATUS_INVALID_DATA;
    }
  }
  payload_bytes = input.length - header_bytes - (size_t)padding_bytes;
  out_packet->header.payload_type = (uint8_t)(data[1] & 0x7fU);
  out_packet->header.marker = (uint8_t)((data[1] >> 7) & 0x01U);
  out_packet->header.sequence_number = aula_rtp_wire_read_u16(data + 2U);
  out_packet->header.timestamp = aula_rtp_wire_read_u32(data + 4U);
  out_packet->header.ssrc = aula_rtp_wire_read_u32(data + 8U);
  out_packet->header.header_bytes = (uint16_t)header_bytes;
  out_packet->payload.data = data + header_bytes;
  out_packet->payload.length = payload_bytes;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_wire_serialize(const aula_rtp_packet *packet,
                                      aula_mutable_bytes *output) {
  size_t output_length;
  if (packet == NULL || output == NULL ||
      (packet->payload.length != 0U && packet->payload.data == NULL) ||
      (output->capacity != 0U && output->data == NULL)) return AULA_STATUS_INVALID_ARGUMENT;
  if (packet->header.payload_type > 127U || packet->header.marker > 1U ||
      (packet->header.header_bytes != 0U &&
       packet->header.header_bytes != AULA_RTP_FIXED_HEADER_BYTES)) return AULA_STATUS_INVALID_ARGUMENT;
  if (packet->payload.length > AULA_SIPD_MAX_RTP_PACKET_BYTES - AULA_RTP_FIXED_HEADER_BYTES) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  output_length = AULA_RTP_FIXED_HEADER_BYTES + packet->payload.length;
  if (output->capacity < output_length) return AULA_STATUS_LIMIT_EXCEEDED;
  output->data[0] = 0x80U;
  output->data[1] = (uint8_t)((packet->header.marker << 7) | packet->header.payload_type);
  aula_rtp_wire_write_u16(output->data + 2U, packet->header.sequence_number);
  aula_rtp_wire_write_u32(output->data + 4U, packet->header.timestamp);
  aula_rtp_wire_write_u32(output->data + 8U, packet->header.ssrc);
  if (packet->payload.length != 0U) {
    (void)memcpy(output->data + AULA_RTP_FIXED_HEADER_BYTES, packet->payload.data,
                 packet->payload.length);
  }
  output->length = output_length;
  return AULA_STATUS_OK;
}
