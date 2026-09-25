#ifndef LS200_SIPD_RX_SHIM_H
#define LS200_SIPD_RX_SHIM_H

#include "ls200_sipd/rtcp.h"
#include "ls200_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_rx_payload_clock {
  uint8_t payload_type;
  uint32_t clock_rate;
} ls200_rx_payload_clock;

/* RTP payload types are scoped to a media section.  A profile therefore
 * accompanies each stream ingress call instead of being global shim state. */
typedef struct ls200_rx_media_profile {
  uint32_t media_id;
  const ls200_rx_payload_clock *payload_clocks;
  size_t payload_clock_count;
} ls200_rx_media_profile;

typedef struct ls200_rx_shim_config {
  const uint8_t *accepted_payload_types;
  size_t accepted_payload_type_count;
  uint32_t maximum_packet_bytes;
  uint32_t maximum_ssrcs;
  uint32_t local_ssrc;
  uint32_t feedback_video_ssrc;
  const char *local_cname;
  uint32_t report_interval_ms;
  int symmetric_rtp_enabled;
} ls200_rx_shim_config;

typedef struct ls200_rx_ssrc_stats {
  uint32_t ssrc;
  ls200_rtp_counters rtp;
  uint64_t bitrate_bps;
  uint64_t last_seen_ns;
  uint64_t rtcp_last_seen_ns;
  uint32_t rtp_clock_rate;
  int source_locked;
  int remote_bye_received;
} ls200_rx_ssrc_stats;

typedef struct ls200_rx_shim_state {
  uint32_t local_ssrc;
  uint64_t last_report_ns;
  uint64_t last_bye_ns;
  uint32_t active_ssrcs;
  uint64_t accepted_feedback;
  uint64_t rejected_feedback;
  uint64_t rate_limited_feedback;
  int started;
  int bye_sent;
  int rx_rendering;
} ls200_rx_shim_state;

typedef struct ls200_rx_shim ls200_rx_shim;

/* The shim validates, accounts for, and drains media. It never decodes or renders. */
ls200_status ls200_rx_shim_create(const ls200_rx_shim_config *config,
                                  ls200_rx_shim **out_shim);
ls200_status ls200_rx_shim_start(ls200_rx_shim *shim);
/* Legacy fixture/video ingress uses a 90 kHz clock. Mixed-media callers must
 * use ls200_rx_shim_accept_rtp_for_media with negotiated clocks. */
ls200_status ls200_rx_shim_accept_rtp(ls200_rx_shim *shim,
                                      const ls200_rtp_source *source,
                                      ls200_bytes packet);
ls200_status ls200_rx_shim_accept_rtp_for_media(ls200_rx_shim *shim,
                                                const ls200_rx_media_profile *profile,
                                                const ls200_rtp_source *source,
                                                ls200_bytes packet);
ls200_status ls200_rx_shim_accept_rtcp(ls200_rx_shim *shim,
                                       const ls200_rtp_source *source,
                                       ls200_bytes packet,
                                       ls200_rtcp_compound_summary *out_summary);
ls200_status ls200_rx_shim_accept_rtcp_for_media(ls200_rx_shim *shim,
                                                 uint32_t media_id,
                                                 const ls200_rtp_source *source,
                                                 ls200_bytes packet,
                                                 ls200_rtcp_compound_summary *out_summary);
/* Legacy diagnostics select the first matching SSRC.  Media-aware callers
 * must provide the media identity because separate RTP sessions may reuse it. */
ls200_status ls200_rx_shim_get_ssrc_stats(const ls200_rx_shim *shim,
                                          uint32_t ssrc,
                                          ls200_rx_ssrc_stats *out_stats);
ls200_status ls200_rx_shim_get_ssrc_stats_for_media(const ls200_rx_shim *shim,
                                                     uint32_t media_id, uint32_t ssrc,
                                                     ls200_rx_ssrc_stats *out_stats);
ls200_status ls200_rx_shim_build_receiver_report(ls200_rx_shim *shim,
                                                 uint32_t local_ssrc,
                                                 ls200_mutable_bytes *output);
/* Supplies a single source's RFC 3550 metrics to the stream-owned reporter.
 * The caller commits the interval only after that reporter has emitted it. */
ls200_status ls200_rx_shim_get_receiver_report_metrics(ls200_rx_shim *shim,
                                                        uint32_t report_block_ssrc,
                                                        uint32_t local_ssrc,
                                                        ls200_rtcp_report_metrics *out_metrics);
ls200_status ls200_rx_shim_get_receiver_report_metrics_for_media(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc,
    uint32_t local_ssrc, ls200_rtcp_report_metrics *out_metrics);
ls200_status ls200_rx_shim_commit_receiver_report(ls200_rx_shim *shim,
                                                   uint32_t report_block_ssrc);
ls200_status ls200_rx_shim_commit_receiver_report_for_media(ls200_rx_shim *shim,
                                                             uint32_t media_id,
                                                             uint32_t report_block_ssrc);
/* Stateful report uses the configured local SSRC and rejects a second BYE. */
ls200_status ls200_rx_shim_build_report(ls200_rx_shim *shim,
                                        ls200_mutable_bytes *output);
ls200_status ls200_rx_shim_get_state(const ls200_rx_shim *shim,
                                     ls200_rx_shim_state *out_state);
ls200_status ls200_rx_shim_stop(ls200_rx_shim *shim, ls200_mutable_bytes *out_bye);
void ls200_rx_shim_destroy(ls200_rx_shim *shim);

#ifdef __cplusplus
}
#endif

#endif
