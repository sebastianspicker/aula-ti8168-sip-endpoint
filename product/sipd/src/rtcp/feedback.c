#include "feedback.h"
#include "framing.h"

#include <string.h>

static ls200_status feedback_tmmbr_rate(uint32_t packed, uint64_t *rate) {
  uint32_t exponent = packed >> 26;
  uint64_t mantissa = (packed >> 9) & 0x1ffffU;
  if (mantissa > (UINT64_MAX >> exponent)) return LS200_STATUS_INVALID_DATA;
  *rate = mantissa << exponent;
  return LS200_STATUS_OK;
}

static ls200_status feedback_validate_rates(ls200_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    ls200_rtcp_frame frame;
    size_t entry;
    ls200_status status = ls200_rtcp_frame_next(input, &offset, &frame);
    if (status != LS200_STATUS_OK) return status;
    if (frame.packet_type != LS200_RTCP_RTPFB || frame.count != 3U) continue;
    for (entry = 12U; entry < frame.content_length; entry += 8U) {
      uint64_t rate;
      status = feedback_tmmbr_rate(ls200_rtcp_frame_read_u32(frame.data + entry + 4U), &rate);
      if (status != LS200_STATUS_OK) return status;
    }
  }
  return LS200_STATUS_OK;
}

static void feedback_visit_frame(const ls200_rtcp_frame *frame,
    ls200_rtcp_feedback_visitor visitor, void *context) {
  ls200_rtcp_feedback_event event;
  size_t entry;
  size_t step;
  (void)memset(&event, 0, sizeof(event));
  if (frame->packet_type == LS200_RTCP_RTPFB && frame->count == 1U) {
    event.kind = LS200_RTCP_FEEDBACK_NACK;
    step = 4U;
  } else if (frame->packet_type == LS200_RTCP_RTPFB && frame->count == 3U) {
    event.kind = LS200_RTCP_FEEDBACK_TMMBR;
    step = 8U;
  } else if (frame->packet_type == LS200_RTCP_PSFB && frame->count == 4U) {
    event.kind = LS200_RTCP_FEEDBACK_FIR;
    step = 8U;
  } else if (frame->packet_type == LS200_RTCP_PSFB && frame->count == 1U) {
    event.kind = LS200_RTCP_FEEDBACK_PLI;
    event.sender_ssrc = ls200_rtcp_frame_read_u32(frame->data + 4U);
    event.media_ssrc = ls200_rtcp_frame_read_u32(frame->data + 8U);
    visitor(&event, context);
    return;
  } else return;
  event.sender_ssrc = ls200_rtcp_frame_read_u32(frame->data + 4U);
  for (entry = 12U; entry < frame->content_length; entry += step) {
    if (event.kind == LS200_RTCP_FEEDBACK_NACK) {
      event.media_ssrc = ls200_rtcp_frame_read_u32(frame->data + 8U);
      event.nack_pid = ls200_rtcp_frame_read_u16(frame->data + entry);
      event.nack_blp = ls200_rtcp_frame_read_u16(frame->data + entry + 2U);
    } else {
      event.media_ssrc = ls200_rtcp_frame_read_u32(frame->data + entry);
      if (event.kind == LS200_RTCP_FEEDBACK_FIR) event.fir_sequence = frame->data[entry + 4U];
      else {
        uint32_t packed = ls200_rtcp_frame_read_u32(frame->data + entry + 4U);
        (void)feedback_tmmbr_rate(packed, &event.bitrate_bps);
        event.overhead_bytes = (uint16_t)(packed & 0x1ffU);
      }
    }
    visitor(&event, context);
  }
}

ls200_status ls200_rtcp_visit_feedback(ls200_bytes input,
    ls200_rtcp_feedback_visitor visitor, void *context) {
  ls200_rtcp_compound_summary summary;
  size_t offset = 0U;
  ls200_status status;
  status = ls200_rtcp_parse_compound(input, &summary);
  if (status != LS200_STATUS_OK) return status;
  status = feedback_validate_rates(input);
  if (status != LS200_STATUS_OK) return status;
  if (visitor == NULL) return LS200_STATUS_OK;
  while (offset < input.length) {
    ls200_rtcp_frame frame;
    status = ls200_rtcp_frame_next(input, &offset, &frame);
    if (status != LS200_STATUS_OK) return status;
    feedback_visit_frame(&frame, visitor, context);
  }
  return LS200_STATUS_OK;
}
