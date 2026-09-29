#include "aula_sipd/rtcp.h"

#include "aula_sipd/platform.h"
#include "feedback.h"

#include <stdlib.h>
#include <string.h>

#define AULA_RTCP_CNAME_BYTES 256U
#define AULA_RTCP_FEEDBACK_SSRC_SLOTS 8U

typedef struct aula_rtcp_feedback_slot {
  uint32_t media_ssrc;
  uint64_t last_accepted_ns;
} aula_rtcp_feedback_slot;

struct aula_rtcp_reporter {
  aula_rtcp_reporter_config config;
  char cname[AULA_RTCP_CNAME_BYTES];
  aula_rtcp_reporter_state state;
  aula_rtcp_feedback_slot feedback_slots[AULA_RTCP_FEEDBACK_SSRC_SLOTS];
  size_t next_feedback_slot;
};

static int aula_rtcp_cname_is_valid(const char *cname) {
  size_t index;
  if (cname == NULL || cname[0] == '\0' || strlen(cname) >= AULA_RTCP_CNAME_BYTES) return 0;
  for (index = 0U; cname[index] != '\0'; ++index) {
    unsigned char c = (unsigned char)cname[index];
    if (c < 0x21U || c > 0x7eU) return 0;
  }
  return 1;
}

static void aula_rtcp_reporter_record_time(uint64_t *out_time) {
  uint64_t now = 0U;
  if (aula_platform_monotonic_now(&now) == AULA_STATUS_OK) *out_time = now;
}

static aula_status aula_rtcp_reporter_is_due(const aula_rtcp_reporter *reporter,
                                               uint64_t previous_report_ns) {
  uint64_t now_ns;
  uint64_t interval_ns;
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) return AULA_STATUS_IO_ERROR;
  interval_ns = (uint64_t)reporter->config.report_interval_ms * UINT64_C(1000000);
  if (previous_report_ns != 0U && now_ns >= previous_report_ns &&
      now_ns - previous_report_ns < interval_ns) return AULA_STATUS_AGAIN;
  return AULA_STATUS_OK;
}

static int aula_rtcp_reporter_feedback_is_rate_limited(aula_rtcp_reporter *reporter,
                                                        uint32_t media_ssrc) {
  uint64_t now_ns;
  uint64_t interval_ns;
  aula_rtcp_feedback_slot *slot = NULL;
  size_t index;
  if (reporter->config.feedback_minimum_interval_ms == 0U) return 0;
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) return -1;
  interval_ns = (uint64_t)reporter->config.feedback_minimum_interval_ms * UINT64_C(1000000);
  for (index = 0U; index < AULA_RTCP_FEEDBACK_SSRC_SLOTS; ++index) {
    if (reporter->feedback_slots[index].last_accepted_ns != 0U &&
        reporter->feedback_slots[index].media_ssrc == media_ssrc) {
      slot = &reporter->feedback_slots[index];
      break;
    }
  }
  if (slot == NULL) {
    for (index = 0U; index < AULA_RTCP_FEEDBACK_SSRC_SLOTS; ++index) {
      if (reporter->feedback_slots[index].last_accepted_ns == 0U) {
        slot = &reporter->feedback_slots[index];
        break;
      }
    }
  }
  if (slot == NULL) {
    slot = &reporter->feedback_slots[reporter->next_feedback_slot];
    reporter->next_feedback_slot = (reporter->next_feedback_slot + 1U) %
                                   AULA_RTCP_FEEDBACK_SSRC_SLOTS;
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

aula_status aula_rtcp_reporter_create(const aula_rtcp_reporter_config *config,
                                        aula_rtcp_reporter **out_reporter) {
  aula_rtcp_reporter *reporter;
  if (config == NULL || out_reporter == NULL || *out_reporter != NULL || config->local_ssrc == 0U ||
      config->report_interval_ms == 0U || !aula_rtcp_cname_is_valid(config->local_cname)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  reporter = (aula_rtcp_reporter *)calloc(1U, sizeof(*reporter));
  if (reporter == NULL) return AULA_STATUS_INTERNAL_ERROR;
  reporter->config = *config;
  (void)memcpy(reporter->cname, config->local_cname, strlen(config->local_cname) + 1U);
  reporter->config.local_cname = reporter->cname;
  reporter->state.local_ssrc = config->local_ssrc;
  *out_reporter = reporter;
  return AULA_STATUS_OK;
}

aula_status aula_rtcp_reporter_build_sender_report(aula_rtcp_reporter *reporter,
                                                      const aula_rtcp_sender_metrics *metrics,
                                                      aula_mutable_bytes *output) {
  aula_status status;
  if (reporter == NULL || metrics == NULL || metrics->ssrc != reporter->state.local_ssrc) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_rtcp_reporter_is_due(reporter, reporter->state.last_sender_report_ns);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtcp_build_sender_report(metrics, output);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtcp_append_sdes_cname(reporter->state.local_ssrc, reporter->cname, output);
  if (status == AULA_STATUS_OK) aula_rtcp_reporter_record_time(&reporter->state.last_sender_report_ns);
  return status;
}

aula_status aula_rtcp_reporter_build_receiver_report(aula_rtcp_reporter *reporter,
                                                        const aula_rtcp_report_metrics *metrics,
                                                        aula_mutable_bytes *output) {
  aula_rtcp_report_metrics report_metrics;
  aula_status status;
  if (reporter == NULL || metrics == NULL || metrics->report_block_ssrc == 0U) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_rtcp_reporter_is_due(reporter, reporter->state.last_receiver_report_ns);
  if (status != AULA_STATUS_OK) return status;
  report_metrics = *metrics;
  report_metrics.reporter_ssrc = reporter->state.local_ssrc;
  status = aula_rtcp_build_receiver_report(&report_metrics, output);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtcp_append_sdes_cname(reporter->state.local_ssrc, reporter->cname, output);
  if (status == AULA_STATUS_OK) aula_rtcp_reporter_record_time(&reporter->state.last_receiver_report_ns);
  return status;
}

typedef struct reporter_feedback_selection {
  uint32_t target;
  aula_rtcp_feedback *feedback;
} reporter_feedback_selection;

static void reporter_select_feedback(const aula_rtcp_feedback_event *event, void *context) {
  reporter_feedback_selection *selection = (reporter_feedback_selection *)context;
  aula_rtcp_feedback *feedback = selection->feedback;
  if (event->kind != AULA_RTCP_FEEDBACK_PLI && event->kind != AULA_RTCP_FEEDBACK_FIR) return;
  if (selection->target != 0U && selection->target != event->media_ssrc) return;
  if (!feedback->request_pli && !feedback->request_fir) {
    feedback->sender_ssrc = event->sender_ssrc;
    feedback->media_ssrc = event->media_ssrc;
  }
  if (feedback->media_ssrc != event->media_ssrc || feedback->sender_ssrc != event->sender_ssrc) return;
  if (event->kind == AULA_RTCP_FEEDBACK_PLI) feedback->request_pli = 1;
  else feedback->request_fir = 1;
}

aula_status aula_rtcp_reporter_accept_feedback(aula_rtcp_reporter *reporter,
                                                  aula_bytes input,
                                                  aula_rtcp_feedback *out_feedback) {
  aula_status status;
  reporter_feedback_selection selection;
  if (reporter == NULL || out_feedback == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_feedback, 0, sizeof(*out_feedback));
  selection.target = reporter->config.feedback_media_ssrc;
  selection.feedback = out_feedback;
  status = aula_rtcp_visit_feedback(input, reporter_select_feedback, &selection);
  if (status != AULA_STATUS_OK) return status;
  if ((out_feedback->request_pli && !reporter->config.feedback_policy.accept_pli) ||
      (out_feedback->request_fir && !reporter->config.feedback_policy.accept_fir) ||
      (reporter->config.feedback_media_ssrc != 0U &&
       out_feedback->media_ssrc != reporter->config.feedback_media_ssrc) ||
      (!out_feedback->request_pli && !out_feedback->request_fir)) {
    reporter->state.rejected_feedback++;
    return AULA_STATUS_PERMISSION_DENIED;
  }
  status = aula_rtcp_reporter_feedback_is_rate_limited(reporter, out_feedback->media_ssrc);
  if (status < 0) return AULA_STATUS_IO_ERROR;
  if (status > 0) return AULA_STATUS_AGAIN;
  reporter->state.accepted_feedback++;
  return AULA_STATUS_OK;
}

aula_status aula_rtcp_reporter_build_bye(aula_rtcp_reporter *reporter,
                                           aula_mutable_bytes *output) {
  aula_status status;
  if (reporter == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (reporter->state.bye_sent) return AULA_STATUS_STATE_ERROR;
  status = aula_rtcp_build_bye(reporter->state.local_ssrc, output);
  if (status == AULA_STATUS_OK) {
    reporter->state.bye_sent = 1;
    aula_rtcp_reporter_record_time(&reporter->state.last_bye_ns);
  }
  return status;
}

aula_status aula_rtcp_reporter_get_state(const aula_rtcp_reporter *reporter,
                                           aula_rtcp_reporter_state *out_state) {
  if (reporter == NULL || out_state == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_state = reporter->state;
  return AULA_STATUS_OK;
}

void aula_rtcp_reporter_destroy(aula_rtcp_reporter *reporter) {
  if (reporter != NULL) {
    (void)memset(reporter, 0, sizeof(*reporter));
    free(reporter);
  }
}
