#include "ls200_sipd/rtcp.h"

#include <limits.h>
#include <string.h>

#include "framing.h"
#include "feedback.h"

static void ls200_rtcp_write_u16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)(value >> 8);
  data[1] = (uint8_t)value;
}

static void ls200_rtcp_write_u24(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 16);
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)value;
}

static void ls200_rtcp_write_u32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)(value >> 24);
  data[1] = (uint8_t)(value >> 16);
  data[2] = (uint8_t)(value >> 8);
  data[3] = (uint8_t)value;
}

static int ls200_rtcp_output_is_valid(const ls200_mutable_bytes *output) {
  return output != NULL && (output->capacity == 0U || output->data != NULL) &&
         output->length <= output->capacity;
}

static void ls200_rtcp_summary_record(ls200_rtcp_compound_summary *summary,
                                      const ls200_rtcp_frame *frame) {
  switch (frame->packet_type) {
    case LS200_RTCP_SR:
      summary->has_sender_report = 1;
      if (summary->sender_report_ssrc == 0U) {
        uint32_t ntp_middle = ls200_rtcp_frame_read_u32(frame->data + 8U);
        summary->sender_report_ssrc = ls200_rtcp_frame_read_u32(frame->data + 4U);
        summary->sender_report_lsr = (ntp_middle << 16) |
            (ls200_rtcp_frame_read_u32(frame->data + 12U) >> 16);
      }
      break;
    case LS200_RTCP_RR: summary->has_receiver_report = 1; break;
    case LS200_RTCP_BYE: summary->has_bye = 1; break;
    case LS200_RTCP_PSFB:
      if (frame->count == 1U) summary->has_pli = 1;
      else if (frame->count == 4U) summary->has_fir = 1;
      break;
    default: break;
  }
}

ls200_status ls200_rtcp_parse_compound(ls200_bytes input,
                                       ls200_rtcp_compound_summary *out_summary) {
  size_t offset = 0U;
  uint32_t packet_count = 0U;

  if (out_summary == NULL || (input.length != 0U && input.data == NULL)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(out_summary, 0, sizeof(*out_summary));
  if (input.length == 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (input.length > LS200_SIPD_MAX_RTCP_PACKET_BYTES) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  while (offset < input.length) {
    ls200_rtcp_frame frame;
    ls200_status status = ls200_rtcp_frame_next(input, &offset, &frame);
    if (status != LS200_STATUS_OK) {
      return status;
    }
    if (frame.has_padding && offset != input.length) {
      return LS200_STATUS_INVALID_DATA;
    }
    status = ls200_rtcp_frame_validate(&frame);
    if (status != LS200_STATUS_OK) {
      return status;
    }
    if (packet_count == UINT32_MAX) {
      return LS200_STATUS_LIMIT_EXCEEDED;
    }
    packet_count++;
    ls200_rtcp_summary_record(out_summary, &frame);
  }
  out_summary->packet_count = packet_count;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_build_receiver_report(const ls200_rtcp_report_metrics *metrics,
                                              ls200_mutable_bytes *output) {
  uint32_t loss;

  if (metrics == NULL || !ls200_rtcp_output_is_valid(output)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (metrics->reporter_ssrc == 0U || metrics->report_block_ssrc == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (metrics->cumulative_loss < -8388608 || metrics->cumulative_loss > 8388607) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (output->capacity < 32U) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  output->data[0] = 0x81U;
  output->data[1] = LS200_RTCP_RR;
  ls200_rtcp_write_u16(output->data + 2U, 7U);
  ls200_rtcp_write_u32(output->data + 4U, metrics->reporter_ssrc);
  ls200_rtcp_write_u32(output->data + 8U, metrics->report_block_ssrc);
  output->data[12] = metrics->fraction_lost;
  loss = (uint32_t)metrics->cumulative_loss & 0x00ffffffU;
  ls200_rtcp_write_u24(output->data + 13U, loss);
  ls200_rtcp_write_u32(output->data + 16U, metrics->highest_sequence);
  ls200_rtcp_write_u32(output->data + 20U, metrics->jitter);
  ls200_rtcp_write_u32(output->data + 24U, metrics->last_sender_report);
  ls200_rtcp_write_u32(output->data + 28U, metrics->delay_since_sender_report);
  output->length = 32U;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_build_sender_report(const ls200_rtcp_sender_metrics *metrics,
                                            ls200_mutable_bytes *output) {
  if (metrics == NULL || !ls200_rtcp_output_is_valid(output)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (output->capacity < 28U) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  output->data[0] = 0x80U;
  output->data[1] = LS200_RTCP_SR;
  ls200_rtcp_write_u16(output->data + 2U, 6U);
  ls200_rtcp_write_u32(output->data + 4U, metrics->ssrc);
  ls200_rtcp_write_u32(output->data + 8U, (uint32_t)(metrics->ntp_timestamp >> 32));
  ls200_rtcp_write_u32(output->data + 12U, (uint32_t)metrics->ntp_timestamp);
  ls200_rtcp_write_u32(output->data + 16U, metrics->rtp_timestamp);
  ls200_rtcp_write_u32(output->data + 20U, metrics->packet_count);
  ls200_rtcp_write_u32(output->data + 24U, metrics->octet_count);
  output->length = 28U;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_append_sdes_cname(uint32_t ssrc, const char *cname,
                                          ls200_mutable_bytes *compound) {
  size_t cname_length;
  size_t raw_length;
  size_t packet_length;
  size_t start;

  if (cname == NULL || !ls200_rtcp_output_is_valid(compound)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  cname_length = strlen(cname);
  if (cname_length == 0U || cname_length > 255U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if ((compound->length & 3U) != 0U || compound->length > LS200_SIPD_MAX_RTCP_PACKET_BYTES) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  raw_length = 4U + 4U + 2U + cname_length + 1U;
  packet_length = (raw_length + 3U) & ~(size_t)3U;
  if (packet_length > LS200_SIPD_MAX_RTCP_PACKET_BYTES ||
      compound->length > LS200_SIPD_MAX_RTCP_PACKET_BYTES - packet_length ||
      compound->capacity - compound->length < packet_length) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  start = compound->length;
  compound->data[start] = 0x81U;
  compound->data[start + 1U] = LS200_RTCP_SDES;
  ls200_rtcp_write_u16(compound->data + start + 2U, (uint16_t)(packet_length / 4U - 1U));
  ls200_rtcp_write_u32(compound->data + start + 4U, ssrc);
  compound->data[start + 8U] = 1U;
  compound->data[start + 9U] = (uint8_t)cname_length;
  (void)memcpy(compound->data + start + 10U, cname, cname_length);
  compound->data[start + 10U + cname_length] = 0U;
  if (packet_length > raw_length) {
    (void)memset(compound->data + start + raw_length, 0, packet_length - raw_length);
  }
  compound->length += packet_length;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_build_bye(uint32_t ssrc, ls200_mutable_bytes *output) {
  if (!ls200_rtcp_output_is_valid(output)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (output->capacity < 8U) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  output->data[0] = 0x81U;
  output->data[1] = LS200_RTCP_BYE;
  ls200_rtcp_write_u16(output->data + 2U, 1U);
  ls200_rtcp_write_u32(output->data + 4U, ssrc);
  output->length = 8U;
  return LS200_STATUS_OK;
}

static void ls200_rtcp_record_feedback(const ls200_rtcp_feedback_event *event,
                                       void *context) {
  ls200_rtcp_feedback *feedback = (ls200_rtcp_feedback *)context;
  if (event->kind != LS200_RTCP_FEEDBACK_PLI && event->kind != LS200_RTCP_FEEDBACK_FIR) return;
  if (!feedback->request_pli && !feedback->request_fir) {
    feedback->sender_ssrc = event->sender_ssrc;
    feedback->media_ssrc = event->media_ssrc;
  }
  /* The legacy projection represents one sender/target only. Do not combine
   * another target's flags with the first target's identity. */
  if (feedback->sender_ssrc != event->sender_ssrc ||
      feedback->media_ssrc != event->media_ssrc) return;
  if (event->kind == LS200_RTCP_FEEDBACK_PLI) feedback->request_pli = 1;
  else feedback->request_fir = 1;
}

ls200_status ls200_rtcp_get_feedback(ls200_bytes input,
                                     ls200_rtcp_feedback *out_feedback) {
  if (out_feedback == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_feedback, 0, sizeof(*out_feedback));
  return ls200_rtcp_visit_feedback(input, ls200_rtcp_record_feedback, out_feedback);
}

static ls200_status ls200_rtcp_build_pli(const ls200_rtcp_feedback *feedback,
                                         ls200_mutable_bytes *output) {
  if (output->capacity < 12U) return LS200_STATUS_LIMIT_EXCEEDED;
  output->data[0] = 0x81U;
  output->data[1] = LS200_RTCP_PSFB;
  ls200_rtcp_write_u16(output->data + 2U, 2U);
  ls200_rtcp_write_u32(output->data + 4U, feedback->sender_ssrc);
  ls200_rtcp_write_u32(output->data + 8U, feedback->media_ssrc);
  output->length = 12U;
  return LS200_STATUS_OK;
}

static ls200_status ls200_rtcp_build_fir(const ls200_rtcp_feedback *feedback,
                                         ls200_mutable_bytes *output) {
  if (output->capacity < 20U) return LS200_STATUS_LIMIT_EXCEEDED;
  output->data[0] = 0x84U;
  output->data[1] = LS200_RTCP_PSFB;
  ls200_rtcp_write_u16(output->data + 2U, 4U);
  ls200_rtcp_write_u32(output->data + 4U, feedback->sender_ssrc);
  ls200_rtcp_write_u32(output->data + 8U, 0U);
  ls200_rtcp_write_u32(output->data + 12U, feedback->media_ssrc);
  (void)memset(output->data + 16U, 0, 4U);
  output->length = 20U;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_build_feedback(const ls200_rtcp_feedback *feedback,
                                       const ls200_rtcp_feedback_policy *policy,
                                       ls200_mutable_bytes *output) {
  if (feedback == NULL || policy == NULL || output == NULL || output->data == NULL ||
      output->length > output->capacity ||
      (!feedback->request_pli && !feedback->request_fir) ||
      (feedback->request_pli && !policy->emit_pli) ||
      (feedback->request_fir && !policy->emit_fir)) return LS200_STATUS_INVALID_ARGUMENT;
  if (feedback->request_pli && feedback->request_fir) return LS200_STATUS_UNSUPPORTED;
  return feedback->request_pli ? ls200_rtcp_build_pli(feedback, output) :
                                 ls200_rtcp_build_fir(feedback, output);
}
