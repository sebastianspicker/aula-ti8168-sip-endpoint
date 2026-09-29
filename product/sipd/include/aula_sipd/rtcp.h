#ifndef AULA_SIPD_RTCP_H
#define AULA_SIPD_RTCP_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_rtcp_packet_type {
  AULA_RTCP_SR = 200,
  AULA_RTCP_RR = 201,
  AULA_RTCP_SDES = 202,
  AULA_RTCP_BYE = 203,
  AULA_RTCP_RTPFB = 205,
  AULA_RTCP_PSFB = 206
} aula_rtcp_packet_type;

typedef struct aula_rtcp_report_metrics {
  uint32_t reporter_ssrc;
  uint32_t report_block_ssrc;
  uint32_t highest_sequence;
  uint32_t jitter;
  uint32_t last_sender_report;
  uint32_t delay_since_sender_report;
  int32_t cumulative_loss;
  uint8_t fraction_lost;
} aula_rtcp_report_metrics;

typedef struct aula_rtcp_compound_summary {
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
} aula_rtcp_compound_summary;

typedef struct aula_rtcp_sender_metrics {
  uint32_t ssrc;
  uint64_t ntp_timestamp;
  uint32_t rtp_timestamp;
  uint32_t packet_count;
  uint32_t octet_count;
} aula_rtcp_sender_metrics;

typedef struct aula_rtcp_feedback {
  uint32_t sender_ssrc;
  uint32_t media_ssrc;
  int request_pli;
  int request_fir;
} aula_rtcp_feedback;

typedef struct aula_rtcp_feedback_policy {
  int accept_pli;
  int accept_fir;
  int emit_pli;
  int emit_fir;
} aula_rtcp_feedback_policy;

typedef struct aula_rtcp_reporter_config {
  uint32_t local_ssrc;
  const char *local_cname;
  uint32_t report_interval_ms;
  aula_rtcp_feedback_policy feedback_policy;
  uint32_t feedback_media_ssrc;
  /* Zero disables coalescing.  Otherwise each media SSRC accepts at most one
   * inbound PLI/FIR during this monotonic interval. */
  uint32_t feedback_minimum_interval_ms;
} aula_rtcp_reporter_config;

typedef struct aula_rtcp_reporter_state {
  uint32_t local_ssrc;
  uint64_t last_sender_report_ns;
  uint64_t last_receiver_report_ns;
  uint64_t last_bye_ns;
  uint64_t accepted_feedback;
  uint64_t rejected_feedback;
  uint64_t rate_limited_feedback;
  int bye_sent;
} aula_rtcp_reporter_state;

typedef struct aula_rtcp_reporter aula_rtcp_reporter;

aula_status aula_rtcp_parse_compound(aula_bytes input,
                                       aula_rtcp_compound_summary *out_summary);
aula_status aula_rtcp_build_receiver_report(const aula_rtcp_report_metrics *metrics,
                                              aula_mutable_bytes *output);
aula_status aula_rtcp_build_sender_report(const aula_rtcp_sender_metrics *metrics,
                                            aula_mutable_bytes *output);
aula_status aula_rtcp_append_sdes_cname(uint32_t ssrc, const char *cname,
                                          aula_mutable_bytes *compound);
aula_status aula_rtcp_build_bye(uint32_t ssrc, aula_mutable_bytes *output);
aula_status aula_rtcp_get_feedback(aula_bytes input,
                                     aula_rtcp_feedback *out_feedback);
aula_status aula_rtcp_build_feedback(const aula_rtcp_feedback *feedback,
                                       const aula_rtcp_feedback_policy *policy,
                                       aula_mutable_bytes *output);
aula_status aula_rtcp_reporter_create(const aula_rtcp_reporter_config *config,
                                        aula_rtcp_reporter **out_reporter);
aula_status aula_rtcp_reporter_build_sender_report(aula_rtcp_reporter *reporter,
                                                      const aula_rtcp_sender_metrics *metrics,
                                                      aula_mutable_bytes *output);
aula_status aula_rtcp_reporter_build_receiver_report(aula_rtcp_reporter *reporter,
                                                        const aula_rtcp_report_metrics *metrics,
                                                        aula_mutable_bytes *output);
aula_status aula_rtcp_reporter_accept_feedback(aula_rtcp_reporter *reporter,
                                                  aula_bytes input,
                                                  aula_rtcp_feedback *out_feedback);
aula_status aula_rtcp_reporter_build_bye(aula_rtcp_reporter *reporter,
                                           aula_mutable_bytes *output);
aula_status aula_rtcp_reporter_get_state(const aula_rtcp_reporter *reporter,
                                           aula_rtcp_reporter_state *out_state);
void aula_rtcp_reporter_destroy(aula_rtcp_reporter *reporter);

#ifdef __cplusplus
}
#endif

#endif
