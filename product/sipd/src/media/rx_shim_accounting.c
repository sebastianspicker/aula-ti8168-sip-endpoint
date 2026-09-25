#include "rx_shim_private.h"
#include "ls200_sipd/platform.h"
#include <string.h>

void ls200_rx_shim_reject_entry(ls200_rx_shim *shim, uint32_t media_id, uint32_t ssrc) {
  ls200_rx_source_entry *entry = ls200_rx_shim_find_entry(shim, media_id, ssrc);
  if (entry != NULL) {
    entry->stats.rtp.rejected++;
  }
}

int ls200_rx_shim_account_sequence(ls200_rx_source_entry *entry,
                                          uint16_t sequence_number) {
  int32_t delta;
  if (entry->has_sequence == 0) {
    entry->has_sequence = 1;
    entry->base_sequence = sequence_number;
    entry->highest_sequence = sequence_number;
    entry->highest_sequence_extended = sequence_number;
    entry->sequence_window = UINT64_C(1);
    entry->unique_received = 1U;
    return 1;
  }
  delta = (int32_t)(int16_t)(sequence_number - entry->highest_sequence);
  if (delta > 0) {
    if (sequence_number < entry->highest_sequence) entry->sequence_cycles++;
    entry->highest_sequence = sequence_number;
    entry->highest_sequence_extended = (entry->sequence_cycles << 16) | sequence_number;
    if ((uint32_t)delta >= 64U) entry->sequence_window = UINT64_C(1);
    else entry->sequence_window = (entry->sequence_window << (uint32_t)delta) | UINT64_C(1);
    entry->unique_received++;
    return 1;
  } else if (delta == 0) {
    entry->stats.rtp.duplicates++;
    return 0;
  } else {
    uint32_t distance = (uint32_t)(-delta);
    if (distance >= 64U) {
      entry->stats.rtp.reordered++;
      return 0;
    }
    if ((entry->sequence_window & (UINT64_C(1) << distance)) != 0U) {
      entry->stats.rtp.duplicates++;
      return 0;
    }
    entry->sequence_window |= UINT64_C(1) << distance;
    entry->unique_received++;
    entry->stats.rtp.reordered++;
    return 1;
  }
}

void ls200_rx_shim_account_jitter(ls200_rx_source_entry *entry,
                                         uint32_t timestamp, uint64_t now_ns) {
  uint64_t seconds = now_ns / UINT64_C(1000000000);
  uint64_t remainder = now_ns % UINT64_C(1000000000);
  uint32_t arrival = (uint32_t)(seconds * entry->stats.rtp_clock_rate +
      (remainder * entry->stats.rtp_clock_rate) / UINT64_C(1000000000));
  uint32_t transit = arrival - timestamp;
  if (entry->has_transit != 0) {
    int64_t difference = (int64_t)(int32_t)(transit - entry->previous_transit);
    uint64_t absolute_difference = difference < 0 ? (uint64_t)(-difference) : (uint64_t)difference;
    if (absolute_difference >= entry->jitter_timestamp_units) {
      entry->jitter_timestamp_units +=
          (uint32_t)((absolute_difference - entry->jitter_timestamp_units) / 16U);
    } else {
      entry->jitter_timestamp_units -=
          (uint32_t)((entry->jitter_timestamp_units - absolute_difference) / 16U);
    }
  }
  entry->previous_transit = transit;
  entry->has_transit = 1;
  entry->stats.rtp.jitter_ns = ((uint64_t)entry->jitter_timestamp_units * UINT64_C(1000000000)) /
      entry->stats.rtp_clock_rate;
}

void ls200_rx_shim_report_metrics(ls200_rx_source_entry *entry,
                                         uint32_t reporter_ssrc, uint64_t now_ns,
                                         ls200_rtcp_report_metrics *out_metrics) {
  uint64_t expected = (uint64_t)entry->highest_sequence_extended - entry->base_sequence + 1U;
  uint64_t lost = expected > entry->unique_received ? expected - entry->unique_received : 0U;
  uint64_t expected_interval = expected - entry->previous_expected;
  uint64_t received_interval = entry->unique_received - entry->previous_received;
  uint64_t lost_interval = expected_interval > received_interval ?
      expected_interval - received_interval : 0U;
  (void)memset(out_metrics, 0, sizeof(*out_metrics));
  out_metrics->reporter_ssrc = reporter_ssrc;
  out_metrics->report_block_ssrc = entry->stats.ssrc;
  out_metrics->highest_sequence = entry->highest_sequence_extended;
  out_metrics->jitter = entry->jitter_timestamp_units;
  out_metrics->cumulative_loss = lost > INT32_MAX ? INT32_MAX : (int32_t)lost;
  out_metrics->fraction_lost = expected_interval == 0U ? 0U :
      (uint8_t)((lost_interval * 256U) / expected_interval > 255U ? 255U :
          (lost_interval * 256U) / expected_interval);
  out_metrics->last_sender_report = entry->last_sender_report;
  if (entry->last_sender_report != 0U && now_ns >= entry->last_sender_report_received_ns) {
    uint64_t elapsed_ns = now_ns - entry->last_sender_report_received_ns;
    out_metrics->delay_since_sender_report = elapsed_ns > UINT64_MAX / UINT64_C(65536) ?
        UINT32_MAX : (uint32_t)((elapsed_ns * UINT64_C(65536)) / UINT64_C(1000000000));
  }
  entry->stats.rtp.lost = lost;
}

void ls200_rx_shim_commit_report_interval(ls200_rx_source_entry *entry) {
  entry->previous_expected = (uint64_t)entry->highest_sequence_extended - entry->base_sequence + 1U;
  entry->previous_received = entry->unique_received;
}

ls200_status ls200_rx_shim_get_receiver_report_metrics_internal(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc,
    uint32_t local_ssrc, ls200_rtcp_report_metrics *out_metrics);
ls200_status ls200_rx_shim_commit_receiver_report_internal(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc);

ls200_status ls200_rx_shim_build_receiver_report(ls200_rx_shim *shim,
                                                 uint32_t local_ssrc,
                                                 ls200_mutable_bytes *output) {
  uint32_t index;
  ls200_rtcp_report_metrics metrics;
  if (shim == NULL || output == NULL || output->data == NULL || shim->started == 0) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (shim->local_ssrc != 0U && local_ssrc != shim->local_ssrc) return LS200_STATUS_PERMISSION_DENIED;
  for (index = 0U; index < shim->maximum_ssrcs; index++) {
    if (shim->entries[index].active != 0) {
      ls200_status status = ls200_rx_shim_get_receiver_report_metrics_internal(
          shim, shim->entries[index].media_id, shim->entries[index].stats.ssrc,
          local_ssrc, &metrics);
      if (status != LS200_STATUS_OK) return status;
      {
        status = ls200_rtcp_build_receiver_report(&metrics, output);
        if (status != LS200_STATUS_OK) return status;
      }
      return ls200_rx_shim_commit_receiver_report_internal(
          shim, shim->entries[index].media_id, shim->entries[index].stats.ssrc);
    }
  }
  return LS200_STATUS_AGAIN;
}
ls200_status ls200_rx_shim_get_receiver_report_metrics_internal(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc,
    uint32_t local_ssrc, ls200_rtcp_report_metrics *out_metrics) {
  ls200_rx_source_entry *entry;
  uint64_t now_ns = 0U;
  if (shim == NULL || out_metrics == NULL || local_ssrc == 0U || shim->started == 0) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  entry = ls200_rx_shim_find_entry(shim, media_id, report_block_ssrc);
  if (entry == NULL || entry->stats.remote_bye_received != 0) return LS200_STATUS_AGAIN;
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK) return LS200_STATUS_IO_ERROR;
  ls200_rx_shim_report_metrics(entry, local_ssrc, now_ns, out_metrics);
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_get_receiver_report_metrics(ls200_rx_shim *shim,
                                                        uint32_t report_block_ssrc,
                                                        uint32_t local_ssrc,
                                                        ls200_rtcp_report_metrics *out_metrics) {
  return ls200_rx_shim_get_receiver_report_metrics_internal(shim, 0U, report_block_ssrc,
                                                             local_ssrc, out_metrics);
}

ls200_status ls200_rx_shim_get_receiver_report_metrics_for_media(
    ls200_rx_shim *shim, uint32_t media_id, uint32_t report_block_ssrc,
    uint32_t local_ssrc, ls200_rtcp_report_metrics *out_metrics) {
  if (media_id == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  return ls200_rx_shim_get_receiver_report_metrics_internal(shim, media_id, report_block_ssrc,
                                                             local_ssrc, out_metrics);
}

ls200_status ls200_rx_shim_commit_receiver_report_internal(ls200_rx_shim *shim,
                                                                    uint32_t media_id,
                                                                    uint32_t report_block_ssrc) {
  ls200_rx_source_entry *entry;
  if (shim == NULL || shim->started == 0) return LS200_STATUS_INVALID_ARGUMENT;
  entry = ls200_rx_shim_find_entry(shim, media_id, report_block_ssrc);
  if (entry == NULL || entry->stats.remote_bye_received != 0) return LS200_STATUS_AGAIN;
  ls200_rx_shim_commit_report_interval(entry);
  (void)ls200_platform_monotonic_now(&shim->last_report_ns);
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_commit_receiver_report(ls200_rx_shim *shim,
                                                   uint32_t report_block_ssrc) {
  return ls200_rx_shim_commit_receiver_report_internal(shim, 0U, report_block_ssrc);
}

ls200_status ls200_rx_shim_commit_receiver_report_for_media(ls200_rx_shim *shim,
                                                             uint32_t media_id,
                                                             uint32_t report_block_ssrc) {
  if (media_id == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  return ls200_rx_shim_commit_receiver_report_internal(shim, media_id, report_block_ssrc);
}

ls200_status ls200_rx_shim_build_report(ls200_rx_shim *shim,
                                        ls200_mutable_bytes *output) {
  uint32_t index;
  ls200_rtcp_report_metrics metrics;
  ls200_status status;
  uint64_t now_ns = 0U;
  if (shim == NULL || output == NULL || shim->started == 0) return LS200_STATUS_INVALID_ARGUMENT;
  if (shim->reporter == NULL || shim->local_ssrc == 0U) return LS200_STATUS_CONFIGURATION_ERROR;
  for (index = 0U; index < shim->maximum_ssrcs; ++index) {
    if (shim->entries[index].active == 0) continue;
    status = ls200_rx_shim_get_receiver_report_metrics_internal(
        shim, shim->entries[index].media_id, shim->entries[index].stats.ssrc,
        shim->local_ssrc, &metrics);
    if (status != LS200_STATUS_OK) return status;
    status = ls200_rtcp_reporter_build_receiver_report(shim->reporter, &metrics, output);
    if (status == LS200_STATUS_OK && ls200_platform_monotonic_now(&now_ns) == LS200_STATUS_OK) {
      shim->last_report_ns = now_ns;
      (void)ls200_rx_shim_commit_receiver_report_internal(
          shim, shim->entries[index].media_id, shim->entries[index].stats.ssrc);
    }
    return status;
  }
  return LS200_STATUS_AGAIN;
}
