#include "feedback.h"
#include "framing.h"

#include <string.h>

static aula_status feedback_tmmbr_rate(uint32_t packed, uint64_t *rate) {
  uint32_t exponent = packed >> 26;
  uint64_t mantissa = (packed >> 9) & 0x1ffffU;
  if (mantissa > (UINT64_MAX >> exponent)) return AULA_STATUS_INVALID_DATA;
  *rate = mantissa << exponent;
  return AULA_STATUS_OK;
}

static aula_status feedback_validate_rates(aula_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    aula_rtcp_frame frame;
    size_t entry;
    aula_status status = aula_rtcp_frame_next(input, &offset, &frame);
    if (status != AULA_STATUS_OK) return status;
    if (frame.packet_type != AULA_RTCP_RTPFB || frame.count != 3U) continue;
    for (entry = 12U; entry < frame.content_length; entry += 8U) {
      uint64_t rate;
      status = feedback_tmmbr_rate(aula_rtcp_frame_read_u32(frame.data + entry + 4U), &rate);
      if (status != AULA_STATUS_OK) return status;
    }
  }
  return AULA_STATUS_OK;
}

static void feedback_visit_frame(const aula_rtcp_frame *frame,
    aula_rtcp_feedback_visitor visitor, void *context) {
  aula_rtcp_feedback_event event;
  size_t entry;
  size_t step;
  (void)memset(&event, 0, sizeof(event));
  if (frame->packet_type == AULA_RTCP_RTPFB && frame->count == 1U) {
    event.kind = AULA_RTCP_FEEDBACK_NACK;
    step = 4U;
  } else if (frame->packet_type == AULA_RTCP_RTPFB && frame->count == 3U) {
    event.kind = AULA_RTCP_FEEDBACK_TMMBR;
    step = 8U;
  } else if (frame->packet_type == AULA_RTCP_PSFB && frame->count == 4U) {
    event.kind = AULA_RTCP_FEEDBACK_FIR;
    step = 8U;
  } else if (frame->packet_type == AULA_RTCP_PSFB && frame->count == 1U) {
    event.kind = AULA_RTCP_FEEDBACK_PLI;
    event.sender_ssrc = aula_rtcp_frame_read_u32(frame->data + 4U);
    event.media_ssrc = aula_rtcp_frame_read_u32(frame->data + 8U);
    visitor(&event, context);
    return;
  } else return;
  event.sender_ssrc = aula_rtcp_frame_read_u32(frame->data + 4U);
  for (entry = 12U; entry < frame->content_length; entry += step) {
    if (event.kind == AULA_RTCP_FEEDBACK_NACK) {
      event.media_ssrc = aula_rtcp_frame_read_u32(frame->data + 8U);
      event.nack_pid = aula_rtcp_frame_read_u16(frame->data + entry);
      event.nack_blp = aula_rtcp_frame_read_u16(frame->data + entry + 2U);
    } else {
      event.media_ssrc = aula_rtcp_frame_read_u32(frame->data + entry);
      if (event.kind == AULA_RTCP_FEEDBACK_FIR) event.fir_sequence = frame->data[entry + 4U];
      else {
        uint32_t packed = aula_rtcp_frame_read_u32(frame->data + entry + 4U);
        (void)feedback_tmmbr_rate(packed, &event.bitrate_bps);
        event.overhead_bytes = (uint16_t)(packed & 0x1ffU);
      }
    }
    visitor(&event, context);
  }
}

aula_status aula_rtcp_visit_feedback(aula_bytes input,
    aula_rtcp_feedback_visitor visitor, void *context) {
  aula_rtcp_compound_summary summary;
  size_t offset = 0U;
  aula_status status;
  status = aula_rtcp_parse_compound(input, &summary);
  if (status != AULA_STATUS_OK) return status;
  status = feedback_validate_rates(input);
  if (status != AULA_STATUS_OK) return status;
  if (visitor == NULL) return AULA_STATUS_OK;
  while (offset < input.length) {
    aula_rtcp_frame frame;
    status = aula_rtcp_frame_next(input, &offset, &frame);
    if (status != AULA_STATUS_OK) return status;
    feedback_visit_frame(&frame, visitor, context);
  }
  return AULA_STATUS_OK;
}
