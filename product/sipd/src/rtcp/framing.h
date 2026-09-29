#ifndef AULA_SIPD_RTCP_FRAMING_H
#define AULA_SIPD_RTCP_FRAMING_H

#include "aula_sipd/rtcp.h"

typedef struct aula_rtcp_frame {
  const uint8_t *data;
  size_t packet_length;
  size_t content_length;
  uint8_t packet_type;
  uint8_t count;
  int has_padding;
} aula_rtcp_frame;

uint16_t aula_rtcp_frame_read_u16(const uint8_t *data);
uint32_t aula_rtcp_frame_read_u32(const uint8_t *data);
void aula_rtcp_frame_write_u16(uint8_t *data, uint16_t value);
void aula_rtcp_frame_write_u24(uint8_t *data, uint32_t value);
void aula_rtcp_frame_write_u32(uint8_t *data, uint32_t value);
int aula_rtcp_frame_output_is_valid(const aula_mutable_bytes *output);
aula_status aula_rtcp_frame_next(aula_bytes input, size_t *offset,
                                   aula_rtcp_frame *out_frame);
aula_status aula_rtcp_frame_validate(const aula_rtcp_frame *frame);

#endif
