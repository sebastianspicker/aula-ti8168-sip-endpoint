#ifndef LS200_SIPD_RTCP_H
#define LS200_SIPD_RTCP_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_rtcp_packet_type {
  LS200_RTCP_SR = 200,
  LS200_RTCP_RR = 201,
  LS200_RTCP_SDES = 202,
  LS200_RTCP_BYE = 203,
  LS200_RTCP_RTPFB = 205,
  LS200_RTCP_PSFB = 206
} ls200_rtcp_packet_type;

typedef struct ls200_rtcp_report_metrics {
  uint32_t reporter_ssrc;
  uint32_t report_block_ssrc;
  uint32_t highest_sequence;
  uint32_t jitter;
  uint32_t last_sender_report;
  uint32_t delay_since_sender_report;
  int32_t cumulative_loss;
  uint8_t fraction_lost;
} ls200_rtcp_report_metrics;

typedef struct ls200_rtcp_compound_summary {
  uint32_t packet_count;
  int has_sender_report;
  int has_receiver_report;
  int has_bye;
  int has_pli;
  int has_fir;
  /* The first sender report's synchronization fields, for RFC 3550 LSR/DLSR
   * accounting by a receive-only participant. */
  uint32_t sender_report_ssrc;
  uint32_t sender_report_lsr;
} ls200_rtcp_compound_summary;

typedef struct ls200_rtcp_sender_metrics {
  uint32_t ssrc;
  uint64_t ntp_timestamp;
  uint32_t rtp_timestamp;
  uint32_t packet_count;
  uint32_t octet_count;
} ls200_rtcp_sender_metrics;

typedef struct ls200_rtcp_feedback {
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  int request_pli;
  int request_fir;
} ls200_rtcp_feedback;

typedef struct ls200_rtcp_feedback_policy {
  int accept_pli;
  int accept_fir;
  int emit_pli;
  int emit_fir;
} ls200_rtcp_feedback_policy;

typedef struct ls200_rtcp_reporter_config {
  uint32_t local_ssrc;
  const char *local_cname;
  uint32_t report_interval_ms;
  ls200_rtcp_feedback_policy feedback_policy;
  uint32_t feedback_media_ssrc;
  /* Zero disables coalescing.  Otherwise each media SSRC accepts at most one
   * inbound PLI/FIR during this monotonic interval. */
  uint32_t feedback_minimum_interval_ms;
} ls200_rtcp_reporter_config;

typedef struct ls200_rtcp_reporter_state {
  uint32_t local_ssrc;
  uint64_t last_sender_report_ns;
  uint64_t last_receiver_report_ns;
  uint64_t last_bye_ns;
  uint64_t accepted_feedback;
  uint64_t rejected_feedback;
  uint64_t rate_limited_feedback;
  int bye_sent;
} ls200_rtcp_reporter_state;

typedef struct ls200_rtcp_reporter ls200_rtcp_reporter;

ls200_status ls200_rtcp_parse_compound(ls200_bytes input,
                                       ls200_rtcp_compound_summary *out_summary);
ls200_status ls200_rtcp_build_receiver_report(const ls200_rtcp_report_metrics *metrics,
                                              ls200_mutable_bytes *output);
ls200_status ls200_rtcp_build_sender_report(const ls200_rtcp_sender_metrics *metrics,
                                            ls200_mutable_bytes *output);
ls200_status ls200_rtcp_append_sdes_cname(uint32_t ssrc, const char *cname,
                                          ls200_mutable_bytes *compound);
ls200_status ls200_rtcp_build_bye(uint32_t ssrc, ls200_mutable_bytes *output);
ls200_status ls200_rtcp_get_feedback(ls200_bytes input,
                                     ls200_rtcp_feedback *out_feedback);
ls200_status ls200_rtcp_build_feedback(const ls200_rtcp_feedback *feedback,
                                       const ls200_rtcp_feedback_policy *policy,
                                       ls200_mutable_bytes *output);
ls200_status ls200_rtcp_reporter_create(const ls200_rtcp_reporter_config *config,
                                        ls200_rtcp_reporter **out_reporter);
ls200_status ls200_rtcp_reporter_build_sender_report(ls200_rtcp_reporter *reporter,
                                                      const ls200_rtcp_sender_metrics *metrics,
                                                      ls200_mutable_bytes *output);
ls200_status ls200_rtcp_reporter_build_receiver_report(ls200_rtcp_reporter *reporter,
                                                        const ls200_rtcp_report_metrics *metrics,
                                                        ls200_mutable_bytes *output);
ls200_status ls200_rtcp_reporter_accept_feedback(ls200_rtcp_reporter *reporter,
                                                  ls200_bytes input,
                                                  ls200_rtcp_feedback *out_feedback);
ls200_status ls200_rtcp_reporter_build_bye(ls200_rtcp_reporter *reporter,
                                           ls200_mutable_bytes *output);
ls200_status ls200_rtcp_reporter_get_state(const ls200_rtcp_reporter *reporter,
                                           ls200_rtcp_reporter_state *out_state);
void ls200_rtcp_reporter_destroy(ls200_rtcp_reporter *reporter);

#ifdef __cplusplus
}
#endif

#endif
