#ifndef LS200_SIPD_RTCP_FRAMING_H
#define LS200_SIPD_RTCP_FRAMING_H

#include "ls200_sipd/rtcp.h"

typedef struct ls200_rtcp_frame {
  const uint8_t *data;
  size_t packet_length;
  size_t content_length;
  uint8_t packet_type;
  uint8_t count;
  int has_padding;
} ls200_rtcp_frame;

uint16_t ls200_rtcp_frame_read_u16(const uint8_t *data);
uint32_t ls200_rtcp_frame_read_u32(const uint8_t *data);
void ls200_rtcp_frame_write_u16(uint8_t *data, uint16_t value);
void ls200_rtcp_frame_write_u24(uint8_t *data, uint32_t value);
void ls200_rtcp_frame_write_u32(uint8_t *data, uint32_t value);
int ls200_rtcp_frame_output_is_valid(const ls200_mutable_bytes *output);
ls200_status ls200_rtcp_frame_next(ls200_bytes input, size_t *offset,
                                   ls200_rtcp_frame *out_frame);
ls200_status ls200_rtcp_frame_validate(const ls200_rtcp_frame *frame);

#endif
