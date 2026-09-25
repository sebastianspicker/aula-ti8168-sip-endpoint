#ifndef LS200_RX_SHIM_PRIVATE_H
#define LS200_RX_SHIM_PRIVATE_H

#include "ls200_sipd/rx_shim.h"

#define LS200_RX_SHIM_FEEDBACK_MINIMUM_INTERVAL_MS 250U

typedef struct ls200_rx_source_entry {
  ls200_rx_ssrc_stats stats;
  ls200_rtp_source source;
  uint32_t media_id;
  uint16_t base_sequence;
  uint16_t highest_sequence;
  uint32_t highest_sequence_extended;
  uint32_t sequence_cycles;
  uint64_t unique_received;
  uint64_t previous_expected;
  uint64_t previous_received;
  uint64_t sequence_window;
  uint64_t first_seen_ns;
  uint64_t first_bytes;
  uint64_t previous_arrival_ns;
  uint32_t previous_transit;
  uint32_t jitter_timestamp_units;
  uint32_t last_sender_report;
  uint64_t last_sender_report_received_ns;
  int active;
  int has_sequence;
  int has_transit;
} ls200_rx_source_entry;

struct ls200_rx_shim {
  uint8_t *accepted_payload_types;
  size_t accepted_payload_type_count;
  uint32_t maximum_packet_bytes;
  uint32_t maximum_ssrcs;
  int symmetric_rtp_enabled;
  int started;
  uint32_t local_ssrc;
  uint32_t report_interval_ms;
  ls200_rtcp_reporter *reporter;
  uint64_t last_report_ns;
  uint64_t last_bye_ns;
  int bye_sent;
  ls200_rx_source_entry *entries;
};

ls200_rx_source_entry *ls200_rx_shim_find_entry(ls200_rx_shim *shim,
                                                 uint32_t media_id, uint32_t ssrc);
int ls200_rx_shim_account_sequence(ls200_rx_source_entry *entry,
                                   uint16_t sequence_number);
void ls200_rx_shim_account_jitter(ls200_rx_source_entry *entry,
                                  uint32_t timestamp, uint64_t now_ns);
void ls200_rx_shim_report_metrics(ls200_rx_source_entry *entry,
                                  uint32_t local_ssrc,
                                  uint64_t now_ns,
                                  ls200_rtcp_report_metrics *out_metrics);
void ls200_rx_shim_commit_report_interval(ls200_rx_source_entry *entry);
ls200_status ls200_rx_shim_get_receiver_report_metrics_internal(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t ssrc, uint32_t local_ssrc,
    ls200_rtcp_report_metrics *out_metrics);
ls200_status ls200_rx_shim_commit_receiver_report_internal(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t ssrc);
void ls200_rx_shim_reject_entry(ls200_rx_shim *shim, uint32_t media_id,
                                uint32_t ssrc);

ls200_status ls200_rx_shim_accept_rtcp_source_internal(
    ls200_rx_shim *shim, uint32_t media_id, const ls200_rtp_source *source,
    ls200_bytes packet, ls200_rtcp_compound_summary *summary);

#endif
