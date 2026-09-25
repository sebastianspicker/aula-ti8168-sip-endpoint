#include "ls200_sipd/rtcp.h"

#include "ls200_sipd/platform.h"
#include "feedback.h"

#include <stdlib.h>
#include <string.h>

#define LS200_RTCP_CNAME_BYTES 256U
#define LS200_RTCP_FEEDBACK_SSRC_SLOTS 8U

typedef struct ls200_rtcp_feedback_slot {
  uint32_t media_ssrc;
  uint64_t last_accepted_ns;
} ls200_rtcp_feedback_slot;

struct ls200_rtcp_reporter {
  ls200_rtcp_reporter_config config;
  char cname[LS200_RTCP_CNAME_BYTES];
  ls200_rtcp_reporter_state state;
  ls200_rtcp_feedback_slot feedback_slots[LS200_RTCP_FEEDBACK_SSRC_SLOTS];
  size_t next_feedback_slot;
};

static int ls200_rtcp_cname_is_valid(const char *cname) {
  size_t index;
  if (cname == NULL || cname[0] == '\0' || strlen(cname) >= LS200_RTCP_CNAME_BYTES) return 0;
  for (index = 0U; cname[index] != '\0'; ++index) {
    unsigned char c = (unsigned char)cname[index];
    if (c < 0x21U || c > 0x7eU) return 0;
  }
  return 1;
}

static void ls200_rtcp_reporter_record_time(uint64_t *out_time) {
  uint64_t now = 0U;
  if (ls200_platform_monotonic_now(&now) == LS200_STATUS_OK) *out_time = now;
}

static ls200_status ls200_rtcp_reporter_is_due(const ls200_rtcp_reporter *reporter,
                                               uint64_t previous_report_ns) {
  uint64_t now_ns;
  uint64_t interval_ns;
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK) return LS200_STATUS_IO_ERROR;
  interval_ns = (uint64_t)reporter->config.report_interval_ms * UINT64_C(1000000);
  if (previous_report_ns != 0U && now_ns >= previous_report_ns &&
      now_ns - previous_report_ns < interval_ns) return LS200_STATUS_AGAIN;
  return LS200_STATUS_OK;
}

static int ls200_rtcp_reporter_feedback_is_rate_limited(ls200_rtcp_reporter *reporter,
                                                        uint32_t media_ssrc) {
  uint64_t now_ns;
  uint64_t interval_ns;
  ls200_rtcp_feedback_slot *slot = NULL;
  size_t index;
  if (reporter->config.feedback_minimum_interval_ms == 0U) return 0;
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK) return -1;
  interval_ns = (uint64_t)reporter->config.feedback_minimum_interval_ms * UINT64_C(1000000);
  for (index = 0U; index < LS200_RTCP_FEEDBACK_SSRC_SLOTS; ++index) {
    if (reporter->feedback_slots[index].last_accepted_ns != 0U &&
        reporter->feedback_slots[index].media_ssrc == media_ssrc) {
      slot = &reporter->feedback_slots[index];
      break;
    }
  }
  if (slot == NULL) {
    for (index = 0U; index < LS200_RTCP_FEEDBACK_SSRC_SLOTS; ++index) {
      if (reporter->feedback_slots[index].last_accepted_ns == 0U) {
        slot = &reporter->feedback_slots[index];
        break;
      }
    }
  }
  if (slot == NULL) {
    slot = &reporter->feedback_slots[reporter->next_feedback_slot];
    reporter->next_feedback_slot = (reporter->next_feedback_slot + 1U) %
                                   LS200_RTCP_FEEDBACK_SSRC_SLOTS;
  }
  if (slot->last_accepted_ns != 0U && now_ns >= slot->last_accepted_ns &&
      now_ns - slot->last_accepted_ns < interval_ns) {
    reporter->state.rate_limited_feedback++;
    return 1;
  }
  slot->media_ssrc = media_ssrc;
  slot->last_accepted_ns = now_ns;
  return 0;
}

ls200_status ls200_rtcp_reporter_create(const ls200_rtcp_reporter_config *config,
                                        ls200_rtcp_reporter **out_reporter) {
  ls200_rtcp_reporter *reporter;
  if (config == NULL || out_reporter == NULL || *out_reporter != NULL || config->local_ssrc == 0U ||
      config->report_interval_ms == 0U || !ls200_rtcp_cname_is_valid(config->local_cname)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  reporter = (ls200_rtcp_reporter *)calloc(1U, sizeof(*reporter));
  if (reporter == NULL) return LS200_STATUS_INTERNAL_ERROR;
  reporter->config = *config;
  (void)memcpy(reporter->cname, config->local_cname, strlen(config->local_cname) + 1U);
  reporter->config.local_cname = reporter->cname;
  reporter->state.local_ssrc = config->local_ssrc;
  *out_reporter = reporter;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_reporter_build_sender_report(ls200_rtcp_reporter *reporter,
                                                      const ls200_rtcp_sender_metrics *metrics,
                                                      ls200_mutable_bytes *output) {
  ls200_status status;
  if (reporter == NULL || metrics == NULL || metrics->ssrc != reporter->state.local_ssrc) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_rtcp_reporter_is_due(reporter, reporter->state.last_sender_report_ns);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtcp_build_sender_report(metrics, output);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtcp_append_sdes_cname(reporter->state.local_ssrc, reporter->cname, output);
  if (status == LS200_STATUS_OK) ls200_rtcp_reporter_record_time(&reporter->state.last_sender_report_ns);
  return status;
}

ls200_status ls200_rtcp_reporter_build_receiver_report(ls200_rtcp_reporter *reporter,
                                                        const ls200_rtcp_report_metrics *metrics,
                                                        ls200_mutable_bytes *output) {
  ls200_rtcp_report_metrics report_metrics;
  ls200_status status;
  if (reporter == NULL || metrics == NULL || metrics->report_block_ssrc == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_rtcp_reporter_is_due(reporter, reporter->state.last_receiver_report_ns);
  if (status != LS200_STATUS_OK) return status;
  report_metrics = *metrics;
  report_metrics.reporter_ssrc = reporter->state.local_ssrc;
  status = ls200_rtcp_build_receiver_report(&report_metrics, output);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtcp_append_sdes_cname(reporter->state.local_ssrc, reporter->cname, output);
  if (status == LS200_STATUS_OK) ls200_rtcp_reporter_record_time(&reporter->state.last_receiver_report_ns);
  return status;
}

typedef struct reporter_feedback_selection {
  uint32_t target;
  ls200_rtcp_feedback *feedback;
} reporter_feedback_selection;

static void reporter_select_feedback(const ls200_rtcp_feedback_event *event, void *context) {
  reporter_feedback_selection *selection = (reporter_feedback_selection *)context;
  ls200_rtcp_feedback *feedback = selection->feedback;
  if (event->kind != LS200_RTCP_FEEDBACK_PLI && event->kind != LS200_RTCP_FEEDBACK_FIR) return;
  if (selection->target != 0U && selection->target != event->media_ssrc) return;
  if (!feedback->request_pli && !feedback->request_fir) {
    feedback->sender_ssrc = event->sender_ssrc;
    feedback->media_ssrc = event->media_ssrc;
  }
  if (feedback->media_ssrc != event->media_ssrc || feedback->sender_ssrc != event->sender_ssrc) return;
  if (event->kind == LS200_RTCP_FEEDBACK_PLI) feedback->request_pli = 1;
  else feedback->request_fir = 1;
}

ls200_status ls200_rtcp_reporter_accept_feedback(ls200_rtcp_reporter *reporter,
                                                  ls200_bytes input,
                                                  ls200_rtcp_feedback *out_feedback) {
  ls200_status status;
  reporter_feedback_selection selection;
  if (reporter == NULL || out_feedback == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_feedback, 0, sizeof(*out_feedback));
  selection.target = reporter->config.feedback_media_ssrc;
  selection.feedback = out_feedback;
  status = ls200_rtcp_visit_feedback(input, reporter_select_feedback, &selection);
  if (status != LS200_STATUS_OK) return status;
  if ((out_feedback->request_pli && !reporter->config.feedback_policy.accept_pli) ||
      (out_feedback->request_fir && !reporter->config.feedback_policy.accept_fir) ||
      (reporter->config.feedback_media_ssrc != 0U &&
       out_feedback->media_ssrc != reporter->config.feedback_media_ssrc) ||
      (!out_feedback->request_pli && !out_feedback->request_fir)) {
    reporter->state.rejected_feedback++;
    return LS200_STATUS_PERMISSION_DENIED;
  }
  status = ls200_rtcp_reporter_feedback_is_rate_limited(reporter, out_feedback->media_ssrc);
  if (status < 0) return LS200_STATUS_IO_ERROR;
  if (status > 0) return LS200_STATUS_AGAIN;
  reporter->state.accepted_feedback++;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtcp_reporter_build_bye(ls200_rtcp_reporter *reporter,
                                           ls200_mutable_bytes *output) {
  ls200_status status;
  if (reporter == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (reporter->state.bye_sent) return LS200_STATUS_STATE_ERROR;
  status = ls200_rtcp_build_bye(reporter->state.local_ssrc, output);
  if (status == LS200_STATUS_OK) {
    reporter->state.bye_sent = 1;
    ls200_rtcp_reporter_record_time(&reporter->state.last_bye_ns);
  }
  return status;
}

ls200_status ls200_rtcp_reporter_get_state(const ls200_rtcp_reporter *reporter,
                                           ls200_rtcp_reporter_state *out_state) {
  if (reporter == NULL || out_state == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_state = reporter->state;
  return LS200_STATUS_OK;
}

void ls200_rtcp_reporter_destroy(ls200_rtcp_reporter *reporter) {
  if (reporter != NULL) {
    (void)memset(reporter, 0, sizeof(*reporter));
    free(reporter);
  }
}
