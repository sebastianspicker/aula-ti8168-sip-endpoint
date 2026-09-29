#include "framing.h"

#define AULA_RTCP_HEADER_BYTES 4U

uint16_t aula_rtcp_frame_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

uint32_t aula_rtcp_frame_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

void aula_rtcp_frame_write_u16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

void aula_rtcp_frame_write_u24(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 16);
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)value;
}

void aula_rtcp_frame_write_u32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

static aula_status aula_rtcp_validate_sdes(const uint8_t *data, size_t length,
                                             uint8_t source_count) {
  size_t offset = AULA_RTCP_HEADER_BYTES;
  uint8_t chunk_count = 0U;
  if (source_count == 0U) return AULA_STATUS_INVALID_DATA;
  while (chunk_count < source_count) {
    size_t chunk_start = offset;
    if (length - offset < 4U) return AULA_STATUS_INVALID_DATA;
    offset += 4U;
    for (;;) {
      uint8_t item_type;
      uint8_t item_length;
      if (offset >= length) return AULA_STATUS_INVALID_DATA;
      item_type = data[offset++];
      if (item_type == 0U) break;
      if (offset >= length) return AULA_STATUS_INVALID_DATA;
      item_length = data[offset++];
      if ((size_t)item_length > length - offset) return AULA_STATUS_INVALID_DATA;
      offset += item_length;
    }
    while (((offset - chunk_start) & 3U) != 0U) {
      if (offset >= length || data[offset] != 0U) return AULA_STATUS_INVALID_DATA;
      offset++;
    }
    chunk_count++;
  }
  return offset == length ? AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
}

static aula_status aula_rtcp_validate_bye(const uint8_t *data, size_t length,
                                            uint8_t source_count) {
  size_t offset = AULA_RTCP_HEADER_BYTES + ((size_t)source_count * 4U);
  if (source_count == 0U || offset > length) return AULA_STATUS_INVALID_DATA;
  if (offset == length) return AULA_STATUS_OK;
  if ((size_t)data[offset] > length - offset - 1U) return AULA_STATUS_INVALID_DATA;
  offset += 1U + (size_t)data[offset];
  while (offset < length) {
    if (data[offset++] != 0U) return AULA_STATUS_INVALID_DATA;
  }
  return AULA_STATUS_OK;
}

static aula_status aula_rtcp_validate_fixed_report(const aula_rtcp_frame *frame,
                                                      size_t base_length) {
  size_t expected = base_length + ((size_t)frame->count * 24U);
  return frame->content_length == expected ? AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
}

static aula_status aula_rtcp_validate_psfb(const aula_rtcp_frame *frame) {
  if (frame->content_length < 12U) return AULA_STATUS_INVALID_DATA;
  if (frame->count == 1U) {
    return frame->content_length == 12U ? AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
  }
  if (frame->count == 4U &&
      (frame->content_length < 20U || ((frame->content_length - 12U) & 7U) != 0U)) {
    return AULA_STATUS_INVALID_DATA;
  }
  return AULA_STATUS_OK;
}

static aula_status aula_rtcp_validate_rtpfb(const aula_rtcp_frame *frame) {
  size_t stride;
  if (frame->content_length < 12U) return AULA_STATUS_INVALID_DATA;
  if (frame->count != 1U && frame->count != 3U) return AULA_STATUS_OK;
  stride = frame->count == 1U ? 4U : 8U;
  return frame->content_length >= 12U + stride &&
      (frame->content_length - 12U) % stride == 0U ?
      AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
}

static aula_status aula_rtcp_validate_packet(const aula_rtcp_frame *frame) {
  switch (frame->packet_type) {
    case AULA_RTCP_SR:
      return aula_rtcp_validate_fixed_report(frame, 28U);
    case AULA_RTCP_RR:
      return aula_rtcp_validate_fixed_report(frame, 8U);
    case AULA_RTCP_SDES:
      return aula_rtcp_validate_sdes(frame->data, frame->content_length, frame->count);
    case AULA_RTCP_BYE:
      return aula_rtcp_validate_bye(frame->data, frame->content_length, frame->count);
    case AULA_RTCP_RTPFB:
      return aula_rtcp_validate_rtpfb(frame);
    case AULA_RTCP_PSFB:
      return aula_rtcp_validate_psfb(frame);
    default:
      return AULA_STATUS_OK;
  }
}

int aula_rtcp_frame_output_is_valid(const aula_mutable_bytes *output) {
  return output != NULL && (output->capacity == 0U || output->data != NULL) &&
         output->length <= output->capacity;
}

aula_status aula_rtcp_frame_next(aula_bytes input, size_t *offset,
                                   aula_rtcp_frame *out_frame) {
  const uint8_t *data;
  size_t remaining;
  uint8_t padding_length;
  if (offset == NULL || out_frame == NULL || *offset > input.length) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  remaining = input.length - *offset;
  if (remaining < AULA_RTCP_HEADER_BYTES) return AULA_STATUS_INVALID_DATA;
  data = input.data + *offset;
  if ((data[0] >> 6) != 2U) return AULA_STATUS_INVALID_DATA;
  out_frame->packet_length = ((size_t)aula_rtcp_frame_read_u16(data + 2U) + 1U) * 4U;
  if (out_frame->packet_length < AULA_RTCP_HEADER_BYTES ||
      out_frame->packet_length > remaining) return AULA_STATUS_INVALID_DATA;
  out_frame->has_padding = ((data[0] >> 5) & 1U) != 0U;
  if (out_frame->has_padding) {
    padding_length = data[out_frame->packet_length - 1U];
    if (padding_length == 0U ||
        (size_t)padding_length > out_frame->packet_length - AULA_RTCP_HEADER_BYTES) {
      return AULA_STATUS_INVALID_DATA;
    }
    out_frame->content_length = out_frame->packet_length - (size_t)padding_length;
  } else {
    out_frame->content_length = out_frame->packet_length;
  }
  out_frame->data = data;
  out_frame->packet_type = data[1];
  out_frame->count = (uint8_t)(data[0] & 0x1fU);
  *offset += out_frame->packet_length;
  return AULA_STATUS_OK;
}

aula_status aula_rtcp_frame_validate(const aula_rtcp_frame *frame) {
  if (frame == NULL || frame->data == NULL ||
      frame->packet_type < 192U || frame->packet_type > 223U ||
      frame->content_length < AULA_RTCP_HEADER_BYTES ||
      frame->content_length > frame->packet_length) return AULA_STATUS_INVALID_DATA;
  return aula_rtcp_validate_packet(frame);
}
