#include "rx_shim_private.h"
#include "aula_sipd/platform.h"
#include <stdlib.h>
#include <string.h>

static int aula_rx_shim_payload_type_allowed(const aula_rx_shim *shim,
                                              uint8_t payload_type) {
  size_t index;
  for (index = 0U; index < shim->accepted_payload_type_count; index++) {
    if (shim->accepted_payload_types[index] == payload_type) {
      return 1;
    }
  }
  return 0;
}

static uint32_t aula_rx_shim_profile_clock_rate(const aula_rx_media_profile *profile,
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

static int aula_rx_shim_profile_is_valid(const aula_rx_media_profile *profile) {
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

static int aula_rx_shim_sources_match(const aula_rtp_source *locked,
                                       const aula_rtp_source *candidate,
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
static int aula_rx_shim_rtcp_sources_match(const aula_rtp_source *locked,
                                            const aula_rtp_source *candidate) {
  return locked->ssrc == candidate->ssrc && locked->address_length != 0U &&
      locked->address_length == candidate->address_length &&
      memcmp(locked->address, candidate->address, locked->address_length) == 0;
}

aula_rx_source_entry *aula_rx_shim_find_entry(aula_rx_shim *shim,
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

static aula_rx_source_entry *aula_rx_shim_find_or_add_entry(
    aula_rx_shim *shim, uint32_t media_id, const aula_rtp_source *source, uint64_t now_ns) {
  aula_rx_source_entry *entry = aula_rx_shim_find_entry(shim, media_id, source->ssrc);
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

static int aula_rx_shim_config_valid(const aula_rx_shim_config *config,
                                      const aula_rx_shim **out_shim) {
  return config != NULL && out_shim != NULL && config->accepted_payload_types != NULL &&
      config->accepted_payload_type_count != 0U &&
      config->accepted_payload_type_count <= 128U && config->maximum_packet_bytes >= 12U &&
      config->maximum_packet_bytes <= AULA_SIPD_MAX_RTP_PACKET_BYTES &&
      config->maximum_ssrcs != 0U && config->maximum_ssrcs <= 64U;
}

static void aula_rx_shim_free_unpublished(aula_rx_shim *shim) {
  free(shim->accepted_payload_types);
  free(shim->entries);
  free(shim);
}

aula_status aula_rx_shim_create(const aula_rx_shim_config *config,
                                  aula_rx_shim **out_shim) {
  aula_rx_shim *shim;
  if (!aula_rx_shim_config_valid(config, (const aula_rx_shim **)out_shim)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  shim = (aula_rx_shim *)calloc(1U, sizeof(*shim));
  if (shim == NULL) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  shim->accepted_payload_types = (uint8_t *)malloc(config->accepted_payload_type_count);
  shim->entries = (aula_rx_source_entry *)calloc(config->maximum_ssrcs,
                                                   sizeof(*shim->entries));
  if (shim->accepted_payload_types == NULL || shim->entries == NULL) {
    aula_rx_shim_free_unpublished(shim);
    return AULA_STATUS_INTERNAL_ERROR;
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
    aula_rtcp_reporter_config reporter_config;
    if (config->local_ssrc == 0U || config->local_cname == NULL ||
        config->report_interval_ms == 0U) {
      aula_rx_shim_free_unpublished(shim);
      return AULA_STATUS_INVALID_ARGUMENT;
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
    reporter_config.feedback_minimum_interval_ms = AULA_RX_SHIM_FEEDBACK_MINIMUM_INTERVAL_MS;
    if (aula_rtcp_reporter_create(&reporter_config, &shim->reporter) != AULA_STATUS_OK) {
      aula_rx_shim_free_unpublished(shim);
      return AULA_STATUS_CONFIGURATION_ERROR;
    }
  }
  *out_shim = shim;
  return AULA_STATUS_OK;
}

aula_status aula_rx_shim_start(aula_rx_shim *shim) {
  if (shim == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started != 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  shim->started = 1;
  return AULA_STATUS_OK;
}

static aula_status aula_rx_shim_validate_rtp_input(
    const aula_rx_shim *shim, const aula_rx_media_profile *profile,
    const aula_rtp_source *source, aula_bytes packet) {
  if (shim == NULL || source == NULL || packet.data == NULL ||
      source->address_length == 0U || source->address_length > sizeof(source->address)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (profile != NULL && !aula_rx_shim_profile_is_valid(profile)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) return AULA_STATUS_STATE_ERROR;
  return packet.length > shim->maximum_packet_bytes ? AULA_STATUS_LIMIT_EXCEEDED : AULA_STATUS_OK;
}

static aula_status aula_rx_shim_parse_allowed_rtp(
    const aula_rx_shim *shim, const aula_rx_media_profile *profile,
    const aula_rtp_source *source, aula_bytes packet, aula_rtp_packet *out_packet) {
  aula_status status = aula_rtp_parse(packet, out_packet);
  if (status != AULA_STATUS_OK) return status;
  if (out_packet->header.ssrc != source->ssrc ||
      !aula_rx_shim_payload_type_allowed(shim, out_packet->header.payload_type) ||
      (profile != NULL &&
       aula_rx_shim_profile_clock_rate(profile, out_packet->header.payload_type) == 0U)) {
    return AULA_STATUS_SECURITY_ERROR;
  }
  return AULA_STATUS_OK;
}

static aula_status aula_rx_shim_admit_rtp_source(
    aula_rx_shim *shim, aula_rx_source_entry *entry, const aula_rtp_source *source,
    uint32_t clock_rate) {
  if (!aula_rx_shim_sources_match(&entry->source, source, shim->symmetric_rtp_enabled)) {
    entry->stats.rtp.rejected++;
    return AULA_STATUS_SECURITY_ERROR;
  }
  if (entry->stats.rtp_clock_rate == 0U) {
    entry->stats.rtp_clock_rate = clock_rate;
    if (clock_rate == 0U) return AULA_STATUS_INVALID_DATA;
  } else if (entry->stats.rtp_clock_rate != clock_rate) {
    entry->stats.rtp.rejected++;
    return AULA_STATUS_INVALID_DATA;
  }
  if (entry->stats.remote_bye_received != 0) {
    entry->stats.rtp.rejected++;
    return AULA_STATUS_STATE_ERROR;
  }
  if (shim->symmetric_rtp_enabled != 0) entry->source.port = source->port;
  return AULA_STATUS_OK;
}

static aula_status aula_rx_shim_accept_rtp_internal(aula_rx_shim *shim,
                                                       const aula_rx_media_profile *profile,
                                                       const aula_rtp_source *source,
                                                       aula_bytes packet) {
  aula_rtp_packet parsed;
  aula_rx_source_entry *entry;
  uint32_t media_id = profile == NULL ? 0U : profile->media_id;
  uint32_t clock_rate;
  uint64_t now_ns = 0U;
  aula_status status;
  status = aula_rx_shim_validate_rtp_input(shim, profile, source, packet);
  if (status != AULA_STATUS_OK) {
    if (status != AULA_STATUS_LIMIT_EXCEEDED) return status;
    aula_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  status = aula_rx_shim_parse_allowed_rtp(shim, profile, source, packet, &parsed);
  if (status != AULA_STATUS_OK) {
    aula_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  /* Preserve the legacy single-stream API as a 90 kHz video seam. Integrated
   * audio/video callers must use the media-aware API and negotiated profile. */
  clock_rate = profile == NULL ? 90000U :
      aula_rx_shim_profile_clock_rate(profile, parsed.header.payload_type);
  (void)aula_platform_monotonic_now(&now_ns);
  entry = aula_rx_shim_find_or_add_entry(shim, media_id, source, now_ns);
  if (entry == NULL) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  status = aula_rx_shim_admit_rtp_source(shim, entry, source, clock_rate);
  if (status != AULA_STATUS_OK) return status;
  if (aula_rx_shim_account_sequence(entry, parsed.header.sequence_number) != 0) {
    entry->stats.rtp.packets++;
    entry->stats.rtp.bytes += packet.length;
  }
  entry->stats.last_seen_ns = now_ns;
  if (entry->first_seen_ns != 0U && now_ns > entry->first_seen_ns) {
    uint64_t elapsed_ns = now_ns - entry->first_seen_ns;
    entry->stats.bitrate_bps = ((entry->stats.rtp.bytes - entry->first_bytes) * 8000000000ULL) /
                               elapsed_ns;
  }
  aula_rx_shim_account_jitter(entry, parsed.header.timestamp, now_ns);
  entry->previous_arrival_ns = now_ns;
  return AULA_STATUS_OK;
}

aula_status aula_rx_shim_accept_rtp(aula_rx_shim *shim,
                                      const aula_rtp_source *source,
                                      aula_bytes packet) {
  return aula_rx_shim_accept_rtp_internal(shim, NULL, source, packet);
}

aula_status aula_rx_shim_accept_rtp_for_media(aula_rx_shim *shim,
                                                const aula_rx_media_profile *profile,
                                                const aula_rtp_source *source,
                                                aula_bytes packet) {
  return aula_rx_shim_accept_rtp_internal(shim, profile, source, packet);
}

static aula_status aula_rx_shim_validate_rtcp_input(
    const aula_rx_shim *shim, const aula_rtp_source *source, aula_bytes packet,
    const aula_rtcp_compound_summary *out_summary) {
  if (shim == NULL || source == NULL || out_summary == NULL || packet.data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) return AULA_STATUS_STATE_ERROR;
  return packet.length > AULA_SIPD_MAX_RTCP_PACKET_BYTES ? AULA_STATUS_LIMIT_EXCEEDED :
      AULA_STATUS_OK;
}

static aula_status aula_rx_shim_accept_rtcp_feedback(
    aula_rx_shim *shim, aula_bytes packet, const aula_rtcp_compound_summary *summary) {
  aula_rtcp_feedback feedback;
  if ((summary->has_pli == 0 && summary->has_fir == 0) || shim->reporter == NULL) {
    return AULA_STATUS_OK;
  }
  return aula_rtcp_reporter_accept_feedback(shim->reporter, packet, &feedback);
}

static void aula_rx_shim_update_rtcp_source(aula_rx_source_entry *entry,
                                              const aula_rtcp_compound_summary *summary,
                                              uint64_t now_ns) {
  if (summary->has_sender_report != 0 && summary->sender_report_ssrc == entry->stats.ssrc) {
    entry->last_sender_report = summary->sender_report_lsr;
    entry->last_sender_report_received_ns = now_ns;
  }
  if (summary->has_bye != 0) entry->stats.remote_bye_received = 1;
}

static aula_status aula_rx_shim_accept_rtcp_internal(aula_rx_shim *shim,
                                                        uint32_t media_id,
                                                        const aula_rtp_source *source,
                                                        aula_bytes packet,
                                                        aula_rtcp_compound_summary *out_summary,
                                                        int handle_feedback) {
  aula_rx_source_entry *entry;
  uint64_t now_ns = 0U;
  aula_status status;
  status = aula_rx_shim_validate_rtcp_input(shim, source, packet, out_summary);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtcp_parse_compound(packet, out_summary);
  if (status != AULA_STATUS_OK) {
    aula_rx_shim_reject_entry(shim, media_id, source->ssrc);
    return status;
  }
  entry = aula_rx_shim_find_entry(shim, media_id, source->ssrc);
  if (entry == NULL || !aula_rx_shim_rtcp_sources_match(&entry->source, source)) {
    return AULA_STATUS_SECURITY_ERROR;
  }
  (void)aula_platform_monotonic_now(&now_ns);
  entry->stats.rtcp_last_seen_ns = now_ns;
  status = handle_feedback ? aula_rx_shim_accept_rtcp_feedback(shim, packet, out_summary) :
      AULA_STATUS_OK;
  if (status != AULA_STATUS_OK) return status;
  aula_rx_shim_update_rtcp_source(entry, out_summary, now_ns);
  return AULA_STATUS_OK;
}

aula_status aula_rx_shim_accept_rtcp(aula_rx_shim *shim,
                                       const aula_rtp_source *source,
                                       aula_bytes packet,
                                       aula_rtcp_compound_summary *out_summary) {
  return aula_rx_shim_accept_rtcp_internal(shim, 0U, source, packet, out_summary, 1);
}

aula_status aula_rx_shim_accept_rtcp_for_media(aula_rx_shim *shim,
                                                 uint32_t media_id,
                                                 const aula_rtp_source *source,
                                                 aula_bytes packet,
                                                 aula_rtcp_compound_summary *out_summary) {
  if (media_id == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  return aula_rx_shim_accept_rtcp_internal(shim, media_id, source, packet, out_summary, 1);
}

aula_status aula_rx_shim_get_ssrc_stats(const aula_rx_shim *shim,
                                          uint32_t ssrc,
                                          aula_rx_ssrc_stats *out_stats) {
  uint32_t index;
  if (shim == NULL || out_stats == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0U; index < shim->maximum_ssrcs; index++) {
    if (shim->entries[index].active != 0 && shim->entries[index].stats.ssrc == ssrc) {
      *out_stats = shim->entries[index].stats;
      return AULA_STATUS_OK;
    }
  }
  return AULA_STATUS_END;
}

aula_status aula_rx_shim_get_ssrc_stats_for_media(const aula_rx_shim *shim,
                                                     uint32_t media_id, uint32_t ssrc,
                                                     aula_rx_ssrc_stats *out_stats) {
  uint32_t index;
  if (shim == NULL || out_stats == NULL || media_id == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  for (index = 0U; index < shim->maximum_ssrcs; ++index) {
    if (shim->entries[index].active != 0 && shim->entries[index].media_id == media_id &&
        shim->entries[index].stats.ssrc == ssrc) {
      *out_stats = shim->entries[index].stats;
      return AULA_STATUS_OK;
    }
  }
  return AULA_STATUS_END;
}

aula_status aula_rx_shim_get_state(const aula_rx_shim *shim,
                                     aula_rx_shim_state *out_state) {
  uint32_t index;
  if (shim == NULL || out_state == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_state, 0, sizeof(*out_state));
  out_state->local_ssrc = shim->local_ssrc;
  out_state->last_report_ns = shim->last_report_ns;
  out_state->last_bye_ns = shim->last_bye_ns;
  out_state->started = shim->started;
  out_state->bye_sent = shim->bye_sent;
  out_state->rx_rendering = 0;
  if (shim->reporter != NULL) {
    aula_rtcp_reporter_state reporter_state;
    if (aula_rtcp_reporter_get_state(shim->reporter, &reporter_state) == AULA_STATUS_OK) {
      out_state->accepted_feedback = reporter_state.accepted_feedback;
      out_state->rejected_feedback = reporter_state.rejected_feedback;
      out_state->rate_limited_feedback = reporter_state.rate_limited_feedback;
    }
  }
  for (index = 0U; index < shim->maximum_ssrcs; ++index) {
    if (shim->entries[index].active != 0) out_state->active_ssrcs++;
  }
  return AULA_STATUS_OK;
}

aula_status aula_rx_shim_stop(aula_rx_shim *shim, aula_mutable_bytes *out_bye) {
  if (shim == NULL || out_bye == NULL || out_bye->data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (shim->started == 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (shim->bye_sent != 0) return AULA_STATUS_STATE_ERROR;
  shim->started = 0;
  if (shim->reporter != NULL) {
    aula_status status = aula_rtcp_reporter_build_bye(shim->reporter, out_bye);
    if (status != AULA_STATUS_OK) return status;
  } else {
    aula_status status = aula_rtcp_build_bye(shim->local_ssrc, out_bye);
    if (status != AULA_STATUS_OK) return status;
  }
  shim->bye_sent = 1;
  (void)aula_platform_monotonic_now(&shim->last_bye_ns);
  return AULA_STATUS_OK;
}

void aula_rx_shim_destroy(aula_rx_shim *shim) {
  if (shim != NULL) {
    free(shim->accepted_payload_types);
    free(shim->entries);
    aula_rtcp_reporter_destroy(shim->reporter);
    free(shim);
  }
}

aula_status aula_rx_shim_accept_rtcp_source_internal(
    aula_rx_shim *shim, uint32_t media_id, const aula_rtp_source *source,
    aula_bytes packet, aula_rtcp_compound_summary *summary) {
  if (media_id == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  return aula_rx_shim_accept_rtcp_internal(shim, media_id, source, packet, summary, 0);
}
