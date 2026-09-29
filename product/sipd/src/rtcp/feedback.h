#ifndef AULA_SIPD_RTCP_FEEDBACK_PRIVATE_H
#define AULA_SIPD_RTCP_FEEDBACK_PRIVATE_H

#include "aula_sipd/rtcp.h"

typedef enum aula_rtcp_feedback_kind {
  AULA_RTCP_FEEDBACK_NACK,
  AULA_RTCP_FEEDBACK_PLI,
  AULA_RTCP_FEEDBACK_FIR,
  AULA_RTCP_FEEDBACK_TMMBR
} aula_rtcp_feedback_kind;

typedef struct aula_rtcp_feedback_event {
  aula_rtcp_feedback_kind kind;
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  uint16_t nack_pid;
  uint16_t nack_blp;
  uint8_t fir_sequence;
  uint64_t bitrate_bps;
  uint16_t overhead_bytes;
} aula_rtcp_feedback_event;

typedef void (*aula_rtcp_feedback_visitor)(
    const aula_rtcp_feedback_event *event, void *context);

/* Call only after transport authentication. Validates the entire compound
 * before invoking visitors; unknown feedback formats are ignored. Input must
 * remain unchanged throughout this synchronous call. TMMBR is a request, not
 * confirmation that the encoder has applied a rate. A NULL visitor performs
 * validation only. */
aula_status aula_rtcp_visit_feedback(aula_bytes input,
    aula_rtcp_feedback_visitor visitor, void *context);

#endif
