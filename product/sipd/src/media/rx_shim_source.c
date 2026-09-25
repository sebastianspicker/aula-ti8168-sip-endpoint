#include "rx_shim_private.h"
#include "ls200_sipd/platform.h"
#include <stdlib.h>
#include <string.h>

static int ls200_rx_shim_payload_type_allowed(const ls200_rx_shim *shim,
                                              uint8_t payload_type) {
  size_t index;
  for (index = 0U; index < shim->accepted_payload_type_count; index++) {
    if (shim->accepted_payload_types[index] == payload_type) {
      return 1;
    }
  }
  return 0;
}

static uint32_t ls200_rx_shim_profile_clock_rate(const ls200_rx_media_profile *profile,
                                                  uint8_t payload_type) {
  size_t index;
  if (profile == NULL) return 90000U;
  for (index = 0U; index < profile->payload_clock_count; ++index) {
    if (profile->payload_clocks[index].payload_type == payload_type) {
      return profile->payload_clocks[index].clock_rate;
    }
  }
  return 0U;
}

static int ls200_rx_shim_profile_is_valid(const ls200_rx_media_profile *profile) {
  size_t index;
  size_t prior;
  if (profile == NULL || profile->media_id == 0U || profile->payload_clocks == NULL ||
      profile->payload_clock_count == 0U || profile->payload_clock_count > 128U) {
    return 0;
  }
  for (index = 0U; index < profile->payload_clock_count; ++index) {
    if (profile->payload_clocks[index].payload_type > 127U ||
        profile->payload_clocks[index].clock_rate == 0U) return 0;
    for (prior = 0U; prior < index; ++prior) {
      if (profile->payload_clocks[prior].payload_type ==
              profile->payload_clocks[index].payload_type &&
          profile->payload_clocks[prior].clock_rate !=
              profile->payload_clocks[index].clock_rate) {
        return 0;
      }
    }
  }
  return 1;
}

static int ls200_rx_shim_sources_match(const ls200_rtp_source *locked,
                                       const ls200_rtp_source *candidate,
                                       int symmetric_rtp_enabled) {
  if (locked->ssrc != candidate->ssrc || locked->address_length == 0U ||
      locked->address_length != candidate->address_length ||
      memcmp(locked->address, candidate->address, locked->address_length) != 0) {
    return 0;
  }
  return symmetric_rtp_enabled != 0 || locked->port == candidate->port;
}

/* RTCP is independently addressed in non-mux sessions.  The transport has
 * already authenticated its negotiated RTCP tuple; association here is by
 * SSRC and address, never by the RTP port. */
static int ls200_rx_shim_rtcp_sources_match(const ls200_rtp_source *locked,
                                            const ls200_rtp_source *candidate) {
  return locked->ssrc == candidate->ssrc && locked->address_length != 0U &&
      locked->address_length == candidate->address_length &&
      memcmp(locked->address, candidate->address, locked->address_length) == 0;
}

ls200_rx_source_entry *ls200_rx_shim_find_entry(ls200_rx_shim *shim,
                                                       uint32_t media_id,
                                                       uint32_t ssrc) {
  uint32_t index;
  for (index = 0U; index < shim->maximum_ssrcs; index++) {
    if (shim->entries[index].active != 0 && shim->entries[index].media_id == media_id &&
        shim->entries[index].stats.ssrc == ssrc) {
      return &shim->entries[index];
    }
  }
  return NULL;
}

static ls200_rx_source_entry *ls200_rx_shim_find_or_add_entry(
    ls200_rx_shim *shim, uint32_t media_id, const ls200_rtp_source *source, uint64_t now_ns) {
  ls200_rx_source_entry *entry = ls200_rx_shim_find_entry(shim, media_id, source->ssrc);
  uint32_t index;
  if (entry != NULL) {
    return entry;
  }
  for (index = 0U; index < shim->maximum_ssrcs; index++) {
    if (shim->entries[index].active == 0) {
      entry = &shim->entries[index];
      memset(entry, 0, sizeof(*entry));
      entry->active = 1;
      entry->source = *source;
      entry->media_id = media_id;
      entry->stats.ssrc = source->ssrc;
      entry->stats.source_locked = 1;
      entry->stats.last_seen_ns = now_ns;
      entry->first_seen_ns = now_ns;
      return entry;
    }
  }
  return NULL;
}

static int ls200_rx_shim_config_valid(const ls200_rx_shim_config *config,
                                      const ls200_rx_shim **out_shim) {
  return config != NULL && out_shim != NULL && config->accepted_payload_types != NULL &&
      config->accepted_payload_type_count != 0U &&
      config->accepted_payload_type_count <= 128U && config->maximum_packet_bytes >= 12U &&
      config->maximum_packet_bytes <= LS200_SIPD_MAX_RTP_PACKET_BYTES &&
      config->maximum_ssrcs != 0U && config->maximum_ssrcs <= 64U;
}

static void ls200_rx_shim_free_unpublished(ls200_rx_shim *shim) {
  free(shim->accepted_payload_types);
  free(shim->entries);
  free(shim);
}

ls200_status ls200_rx_shim_create(const ls200_rx_shim_config *config,
                                  ls200_rx_shim **out_shim) {
  ls200_rx_shim *shim;
  if (!ls200_rx_shim_config_valid(config, (const ls200_rx_shim **)out_shim)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  shim = (ls200_rx_shim *)calloc(1U, sizeof(*shim));
  if (shim == NULL) {
    return LS200_STATUS_INTERNAL_ERROR;
  }
  shim->accepted_payload_types = (uint8_t *)malloc(config->accepted_payload_type_count);
  shim->entries = (ls200_rx_source_entry *)calloc(config->maximum_ssrcs,
                                                   sizeof(*shim->entries));
  if (shim->accepted_payload_types == NULL || shim->entries == NULL) {
    ls200_rx_shim_free_unpublished(shim);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  memcpy(shim->accepted_payload_types, config->accepted_payload_types,
         config->accepted_payload_type_count);
  shim->accepted_payload_type_count = config->accepted_payload_type_count;
  shim->maximum_packet_bytes = config->maximum_packet_bytes;
  shim->maximum_ssrcs = config->maximum_ssrcs;
  shim->symmetric_rtp_enabled = config->symmetric_rtp_enabled != 0;
  shim->local_ssrc = config->local_ssrc;
  shim->report_interval_ms = config->report_interval_ms;
  if (config->local_ssrc != 0U || config->local_cname != NULL || config->report_interval_ms != 0U) {
    ls200_rtcp_reporter_config reporter_config;
    if (config->local_ssrc == 0U || config->local_cname == NULL ||
        config->report_interval_ms == 0U) {
      ls200_rx_shim_free_unpublished(shim);
      return LS200_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(&reporter_config, 0, sizeof(reporter_config));
    reporter_config.local_ssrc = config->local_ssrc;
    reporter_config.local_cname = config->local_cname;
    reporter_config.report_interval_ms = config->report_interval_ms;
    reporter_config.feedback_media_ssrc = config->feedback_video_ssrc;
    reporter_config.feedback_policy.accept_pli = 1;
    reporter_config.feedback_policy.accept_fir = 1;
    reporter_config.feedback_policy.emit_pli = 1;
    reporter_config.feedback_policy.emit_fir = 1;
    reporter_config.feedback_minimum_interval_ms = LS200_RX_SHIM_FEEDBACK_MINIMUM_INTERVAL_MS;
    if (ls200_rtcp_reporter_create(&reporter_config, &shim->reporter) != LS200_STATUS_OK) {
      ls200_rx_shim_free_unpublished(shim);
      return LS200_STATUS_CONFIGURATION_ERROR;
    }
  }
  *out_shim = shim;
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_start(ls200_rx_shim *shim) {
  if (shim == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started != 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  shim->started = 1;
  return LS200_STATUS_OK;
}

static ls200_status ls200_rx_shim_validate_rtp_input(
    const ls200_rx_shim *shim, const ls200_rx_media_profile *profile,
    const ls200_rtp_source *source, ls200_bytes packet) {
  if (shim == NULL || source == NULL || packet.data == NULL ||
      source->address_length == 0U || source->address_length > sizeof(source->address)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (profile != NULL && !ls200_rx_shim_profile_is_valid(profile)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) return LS200_STATUS_STATE_ERROR;
  return packet.length > shim->maximum_packet_bytes ? LS200_STATUS_LIMIT_EXCEEDED : LS200_STATUS_OK;
}

static ls200_status ls200_rx_shim_parse_allowed_rtp(
    const ls200_rx_shim *shim, const ls200_rx_media_profile *profile,
    const ls200_rtp_source *source, ls200_bytes packet, ls200_rtp_packet *out_packet) {
  ls200_status status = ls200_rtp_parse(packet, out_packet);
  if (status != LS200_STATUS_OK) return status;
  if (out_packet->header.ssrc != source->ssrc ||
      !ls200_rx_shim_payload_type_allowed(shim, out_packet->header.payload_type) ||
      (profile != NULL &&
       ls200_rx_shim_profile_clock_rate(profile, out_packet->header.payload_type) == 0U)) {
    return LS200_STATUS_SECURITY_ERROR;
  }
  return LS200_STATUS_OK;
}

static ls200_status ls200_rx_shim_admit_rtp_source(
    ls200_rx_shim *shim, ls200_rx_source_entry *entry, const ls200_rtp_source *source,
    uint32_t clock_rate) {
  if (!ls200_rx_shim_sources_match(&entry->source, source, shim->symmetric_rtp_enabled)) {
    entry->stats.rtp.rejected++;
    return LS200_STATUS_SECURITY_ERROR;
  }
  if (entry->stats.rtp_clock_rate == 0U) {
    entry->stats.rtp_clock_rate = clock_rate;
    if (clock_rate == 0U) return LS200_STATUS_INVALID_DATA;
  } else if (entry->stats.rtp_clock_rate != clock_rate) {
    entry->stats.rtp.rejected++;
    return LS200_STATUS_INVALID_DATA;
  }
  if (entry->stats.remote_bye_received != 0) {
    entry->stats.rtp.rejected++;
    return LS200_STATUS_STATE_ERROR;
  }
  if (shim->symmetric_rtp_enabled != 0) entry->source.port = source->port;
  return LS200_STATUS_OK;
}

static ls200_status ls200_rx_shim_accept_rtp_internal(ls200_rx_shim *shim,
                                                       const ls200_rx_media_profile *profile,
                                                       const ls200_rtp_source *source,
                                                       ls200_bytes packet) {
  ls200_rtp_packet parsed;
  ls200_rx_source_entry *entry;
  uint32_t media_id = profile == NULL ? 0U : profile->media_id;
  uint32_t clock_rate;
  uint64_t now_ns = 0U;
  ls200_status status;
  status = ls200_rx_shim_validate_rtp_input(shim, profile, source, packet);
  if (status != LS200_STATUS_OK) {
    if (status != LS200_STATUS_LIMIT_EXCEEDED) return status;
    ls200_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  status = ls200_rx_shim_parse_allowed_rtp(shim, profile, source, packet, &parsed);
  if (status != LS200_STATUS_OK) {
    ls200_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  /* Preserve the legacy single-stream API as a 90 kHz video seam. Integrated
   * audio/video callers must use the media-aware API and negotiated profile. */
  clock_rate = profile == NULL ? 90000U :
      ls200_rx_shim_profile_clock_rate(profile, parsed.header.payload_type);
  (void)ls200_platform_monotonic_now(&now_ns);
  entry = ls200_rx_shim_find_or_add_entry(shim, media_id, source, now_ns);
  if (entry == NULL) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  status = ls200_rx_shim_admit_rtp_source(shim, entry, source, clock_rate);
  if (status != LS200_STATUS_OK) return status;
  if (ls200_rx_shim_account_sequence(entry, parsed.header.sequence_number) != 0) {
    entry->stats.rtp.packets++;
    entry->stats.rtp.bytes += packet.length;
  }
  entry->stats.last_seen_ns = now_ns;
  if (entry->first_seen_ns != 0U && now_ns > entry->first_seen_ns) {
    uint64_t elapsed_ns = now_ns - entry->first_seen_ns;
    entry->stats.bitrate_bps = ((entry->stats.rtp.bytes - entry->first_bytes) * 8000000000ULL) /
                               elapsed_ns;
  }
  ls200_rx_shim_account_jitter(entry, parsed.header.timestamp, now_ns);
  entry->previous_arrival_ns = now_ns;
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_accept_rtp(ls200_rx_shim *shim,
                                      const ls200_rtp_source *source,
                                      ls200_bytes packet) {
  return ls200_rx_shim_accept_rtp_internal(shim, NULL, source, packet);
}

ls200_status ls200_rx_shim_accept_rtp_for_media(ls200_rx_shim *shim,
                                                const ls200_rx_media_profile *profile,
                                                const ls200_rtp_source *source,
                                                ls200_bytes packet) {
  return ls200_rx_shim_accept_rtp_internal(shim, profile, source, packet);
}

static ls200_status ls200_rx_shim_validate_rtcp_input(
    const ls200_rx_shim *shim, const ls200_rtp_source *source, ls200_bytes packet,
    const ls200_rtcp_compound_summary *out_summary) {
  if (shim == NULL || source == NULL || out_summary == NULL || packet.data == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) return LS200_STATUS_STATE_ERROR;
  return packet.length > LS200_SIPD_MAX_RTCP_PACKET_BYTES ? LS200_STATUS_LIMIT_EXCEEDED :
      LS200_STATUS_OK;
}

static ls200_status ls200_rx_shim_accept_rtcp_feedback(
    ls200_rx_shim *shim, ls200_bytes packet, const ls200_rtcp_compound_summary *summary) {
  ls200_rtcp_feedback feedback;
  if ((summary->has_pli == 0 && summary->has_fir == 0) || shim->reporter == NULL) {
    return LS200_STATUS_OK;
  }
  return ls200_rtcp_reporter_accept_feedback(shim->reporter, packet, &feedback);
}

static void ls200_rx_shim_update_rtcp_source(ls200_rx_source_entry *entry,
                                              const ls200_rtcp_compound_summary *summary,
                                              uint64_t now_ns) {
  if (summary->has_sender_report != 0 && summary->sender_report_ssrc == entry->stats.ssrc) {
    entry->last_sender_report = summary->sender_report_lsr;
    entry->last_sender_report_received_ns = now_ns;
  }
  if (summary->has_bye != 0) entry->stats.remote_bye_received = 1;
}

static ls200_status ls200_rx_shim_accept_rtcp_internal(ls200_rx_shim *shim,
                                                        uint32_t media_id,
                                                        const ls200_rtp_source *source,
                                                        ls200_bytes packet,
                                                        ls200_rtcp_compound_summary *out_summary,
                                                        int handle_feedback) {
  ls200_rx_source_entry *entry;
  uint64_t now_ns = 0U;
  ls200_status status;
  status = ls200_rx_shim_validate_rtcp_input(shim, source, packet, out_summary);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtcp_parse_compound(packet, out_summary);
  if (status != LS200_STATUS_OK) {
    ls200_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  entry = ls200_rx_shim_find_entry(shim, media_id, source->ssrc);
  if (entry == NULL || !ls200_rx_shim_rtcp_sources_match(&entry->source, source)) {
    return LS200_STATUS_SECURITY_ERROR;
  }
  (void)ls200_platform_monotonic_now(&now_ns);
  entry->stats.rtcp_last_seen_ns = now_ns;
  status = handle_feedback ? ls200_rx_shim_accept_rtcp_feedback(shim, packet, out_summary) :
      LS200_STATUS_OK;
  if (status != LS200_STATUS_OK) return status;
  ls200_rx_shim_update_rtcp_source(entry, out_summary, now_ns);
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_accept_rtcp(ls200_rx_shim *shim,
                                       const ls200_rtp_source *source,
                                       ls200_bytes packet,
                                       ls200_rtcp_compound_summary *out_summary) {
  return ls200_rx_shim_accept_rtcp_internal(shim, 0U, source, packet, out_summary, 1);
}

ls200_status ls200_rx_shim_accept_rtcp_for_media(ls200_rx_shim *shim,
                                                 uint32_t media_id,
                                                 const ls200_rtp_source *source,
                                                 ls200_bytes packet,
                                                 ls200_rtcp_compound_summary *out_summary) {
  if (media_id == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  return ls200_rx_shim_accept_rtcp_internal(shim, media_id, source, packet, out_summary, 1);
}

ls200_status ls200_rx_shim_get_ssrc_stats(const ls200_rx_shim *shim,
                                          uint32_t ssrc,
                                          ls200_rx_ssrc_stats *out_stats) {
  uint32_t index;
  if (shim == NULL || out_stats == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0U; index < shim->maximum_ssrcs; index++) {
    if (shim->entries[index].active != 0 && shim->entries[index].stats.ssrc == ssrc) {
      *out_stats = shim->entries[index].stats;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_END;
}

ls200_status ls200_rx_shim_get_ssrc_stats_for_media(const ls200_rx_shim *shim,
                                                     uint32_t media_id, uint32_t ssrc,
                                                     ls200_rx_ssrc_stats *out_stats) {
  uint32_t index;
  if (shim == NULL || out_stats == NULL || media_id == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  for (index = 0U; index < shim->maximum_ssrcs; ++index) {
    if (shim->entries[index].active != 0 && shim->entries[index].media_id == media_id &&
        shim->entries[index].stats.ssrc == ssrc) {
      *out_stats = shim->entries[index].stats;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_END;
}

ls200_status ls200_rx_shim_get_state(const ls200_rx_shim *shim,
                                     ls200_rx_shim_state *out_state) {
  uint32_t index;
  if (shim == NULL || out_state == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_state, 0, sizeof(*out_state));
  out_state->local_ssrc = shim->local_ssrc;
  out_state->last_report_ns = shim->last_report_ns;
  out_state->last_bye_ns = shim->last_bye_ns;
  out_state->started = shim->started;
  out_state->bye_sent = shim->bye_sent;
  out_state->rx_rendering = 0;
  if (shim->reporter != NULL) {
    ls200_rtcp_reporter_state reporter_state;
    if (ls200_rtcp_reporter_get_state(shim->reporter, &reporter_state) == LS200_STATUS_OK) {
      out_state->accepted_feedback = reporter_state.accepted_feedback;
      out_state->rejected_feedback = reporter_state.rejected_feedback;
      out_state->rate_limited_feedback = reporter_state.rate_limited_feedback;
    }
  }
  for (index = 0U; index < shim->maximum_ssrcs; ++index) {
    if (shim->entries[index].active != 0) out_state->active_ssrcs++;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_rx_shim_stop(ls200_rx_shim *shim, ls200_mutable_bytes *out_bye) {
  if (shim == NULL || out_bye == NULL || out_bye->data == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  if (shim->bye_sent != 0) return LS200_STATUS_STATE_ERROR;
  shim->started = 0;
  if (shim->reporter != NULL) {
    ls200_status status = ls200_rtcp_reporter_build_bye(shim->reporter, out_bye);
    if (status != LS200_STATUS_OK) return status;
  } else {
    ls200_status status = ls200_rtcp_build_bye(shim->local_ssrc, out_bye);
    if (status != LS200_STATUS_OK) return status;
  }
  shim->bye_sent = 1;
  (void)ls200_platform_monotonic_now(&shim->last_bye_ns);
  return LS200_STATUS_OK;
}

void ls200_rx_shim_destroy(ls200_rx_shim *shim) {
  if (shim != NULL) {
    free(shim->accepted_payload_types);
    free(shim->entries);
    ls200_rtcp_reporter_destroy(shim->reporter);
    free(shim);
  }
}

ls200_status ls200_rx_shim_accept_rtcp_source_internal(
    ls200_rx_shim *shim, uint32_t media_id, const ls200_rtp_source *source,
    ls200_bytes packet, ls200_rtcp_compound_summary *summary) {
  if (media_id == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  return ls200_rx_shim_accept_rtcp_internal(shim, media_id, source, packet, summary, 0);
}
