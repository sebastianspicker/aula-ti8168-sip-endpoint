#ifndef AULA_SIPD_RX_SHIM_H
#define AULA_SIPD_RX_SHIM_H

#include "aula_sipd/rtcp.h"
#include "aula_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_rx_payload_clock {
  uint8_t payload_type;
  uint32_t clock_rate;
} aula_rx_payload_clock;

/* RTP payload types are scoped to a media section.  A profile therefore
 * accompanies each stream ingress call instead of being global shim state. */
typedef struct aula_rx_media_profile {
  uint32_t media_id;
  const aula_rx_payload_clock *payload_clocks;
  size_t payload_clock_count;
} aula_rx_media_profile;

typedef struct aula_rx_shim_config {
  const uint8_t *accepted_payload_types;
  size_t accepted_payload_type_count;
  uint32_t maximum_packet_bytes;
  uint32_t maximum_ssrcs;
  uint32_t local_ssrc;
  uint32_t feedback_video_ssrc;
  const char *local_cname;
  uint32_t report_interval_ms;
  int symmetric_rtp_enabled;
} aula_rx_shim_config;

typedef struct aula_rx_ssrc_stats {
  uint32_t ssrc;
  aula_rtp_counters rtp;
  uint64_t bitrate_bps;
  uint64_t last_seen_ns;
  uint64_t rtcp_last_seen_ns;
  uint32_t rtp_clock_rate;
  int source_locked;
  int remote_bye_received;
} aula_rx_ssrc_stats;

typedef struct aula_rx_shim_state {
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
} aula_rx_shim_state;

typedef struct aula_rx_shim aula_rx_shim;

/* The shim validates, accounts for, and drains media. It never decodes or renders. */
aula_status aula_rx_shim_create(const aula_rx_shim_config *config,
                                  aula_rx_shim **out_shim);
aula_status aula_rx_shim_start(aula_rx_shim *shim);
/* Legacy fixture/video ingress uses a 90 kHz clock. Mixed-media callers must
 * use aula_rx_shim_accept_rtp_for_media with negotiated clocks. */
aula_status aula_rx_shim_accept_rtp(aula_rx_shim *shim,
                                      const aula_rtp_source *source,
                                      aula_bytes packet);
aula_status aula_rx_shim_accept_rtp_for_media(aula_rx_shim *shim,
                                                const aula_rx_media_profile *profile,
                                                const aula_rtp_source *source,
                                                aula_bytes packet);
aula_status aula_rx_shim_accept_rtcp(aula_rx_shim *shim,
                                       const aula_rtp_source *source,
                                       aula_bytes packet,
                                       aula_rtcp_compound_summary *out_summary);
aula_status aula_rx_shim_accept_rtcp_for_media(aula_rx_shim *shim,
                                                 uint32_t media_id,
                                                 const aula_rtp_source *source,
                                                 aula_bytes packet,
                                                 aula_rtcp_compound_summary *out_summary);
/* Legacy diagnostics select the first matching SSRC.  Media-aware callers
 * must provide the media identity because separate RTP sessions may reuse it. */
aula_status aula_rx_shim_get_ssrc_stats(const aula_rx_shim *shim,
                                          uint32_t ssrc,
                                          aula_rx_ssrc_stats *out_stats);
aula_status aula_rx_shim_get_ssrc_stats_for_media(const aula_rx_shim *shim,
                                                     uint32_t media_id, uint32_t ssrc,
                                                     aula_rx_ssrc_stats *out_stats);
aula_status aula_rx_shim_build_receiver_report(aula_rx_shim *shim,
                                                 uint32_t local_ssrc,
                                                 aula_mutable_bytes *output);
/* Supplies a single source's RFC 3550 metrics to the stream-owned reporter.
 * The caller commits the interval only after that reporter has emitted it. */
aula_status aula_rx_shim_get_receiver_report_metrics(aula_rx_shim *shim,
                                                        uint32_t report_block_ssrc,
                                                        uint32_t local_ssrc,
                                                        aula_rtcp_report_metrics *out_metrics);
aula_status aula_rx_shim_get_receiver_report_metrics_for_media(
    aula_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc,
    uint32_t local_ssrc, aula_rtcp_report_metrics *out_metrics);
aula_status aula_rx_shim_commit_receiver_report(aula_rx_shim *shim,
                                                   uint32_t report_block_ssrc);
aula_status aula_rx_shim_commit_receiver_report_for_media(aula_rx_shim *shim,
                                                             uint32_t media_id,
                                                             uint32_t report_block_ssrc);
/* Stateful report uses the configured local SSRC and rejects a second BYE. */
aula_status aula_rx_shim_build_report(aula_rx_shim *shim,
                                        aula_mutable_bytes *output);
aula_status aula_rx_shim_get_state(const aula_rx_shim *shim,
                                     aula_rx_shim_state *out_state);
aula_status aula_rx_shim_stop(aula_rx_shim *shim, aula_mutable_bytes *out_bye);
void aula_rx_shim_destroy(aula_rx_shim *shim);

#ifdef __cplusplus
}
#endif

#endif
