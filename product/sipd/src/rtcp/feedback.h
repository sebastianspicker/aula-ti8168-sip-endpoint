#ifndef LS200_SIPD_RTCP_FEEDBACK_PRIVATE_H
#define LS200_SIPD_RTCP_FEEDBACK_PRIVATE_H

#include "ls200_sipd/rtcp.h"

typedef enum ls200_rtcp_feedback_kind {
  LS200_RTCP_FEEDBACK_NACK,
  LS200_RTCP_FEEDBACK_PLI,
  LS200_RTCP_FEEDBACK_FIR,
  LS200_RTCP_FEEDBACK_TMMBR
} ls200_rtcp_feedback_kind;

typedef struct ls200_rtcp_feedback_event {
  ls200_rtcp_feedback_kind kind;
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  uint16_t nack_pid;
  uint16_t nack_blp;
  uint8_t fir_sequence;
  uint64_t bitrate_bps;
  uint16_t overhead_bytes;
} ls200_rtcp_feedback_event;

typedef void (*ls200_rtcp_feedback_visitor)(
    const ls200_rtcp_feedback_event *event, void *context);

/* Call only after transport authentication. Validates the entire compound
 * before invoking visitors; unknown feedback formats are ignored. Input must
 * remain unchanged throughout this synchronous call. TMMBR is a request, not
 * confirmation that the encoder has applied a rate. A NULL visitor performs
 * validation only. */
ls200_status ls200_rtcp_visit_feedback(ls200_bytes input,
    ls200_rtcp_feedback_visitor visitor, void *context);

#endif
