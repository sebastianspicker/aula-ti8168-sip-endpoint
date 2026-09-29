#include "session_private.h"
#include "aula_sipd/platform.h"
#include "aula_sipd/rtcp.h"
#include "../rtp/session_internal.h"
#include "../rtcp/framing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int aula_media_session_direction_sends(aula_sdp_direction direction) {
  return direction == AULA_SDP_SENDRECV || direction == AULA_SDP_SENDONLY;
}

static int media_session_transport_config_valid(const aula_media_session_config *config) {
  return config->transport.local_address != NULL &&
      (config->transport.local_address_length == 4U ||
       config->transport.local_address_length == 16U) &&
      config->transport.minimum_port != 0U &&
      config->transport.maximum_port >= config->transport.minimum_port &&
      config->rtp_mtu >= 64U && config->rtp_mtu <= AULA_SIPD_MAX_RTP_PACKET_BYTES;
}

static int media_session_queue_config_valid(const aula_media_session_config *config) {
  return config->maximum_video_queue_packets != 0U &&
      config->maximum_audio_queue_packets != 0U &&
      config->maximum_queue_bytes != 0U &&
      config->maximum_queue_bytes <= 4096U * AULA_SIPD_MAX_RTP_PACKET_BYTES &&
      config->maximum_ssrcs != 0U && config->maximum_ssrcs <= 64U &&
      config->report_interval_ms != 0U;
}

static int media_session_config_valid(const aula_media_session_config *config) {
  return config != NULL && config->backend_config != NULL && config->local_cname != NULL &&
         config->advertised_address != NULL && config->advertised_address[0] != '\0' &&
         media_session_transport_config_valid(config) && media_session_queue_config_valid(config);
}

void aula_media_session_record_error(aula_media_session *session, aula_status status) {
  if (session != NULL && status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) {
    session->status.last_error = status;
  }
}

void aula_media_session_record_activity(aula_media_session *session) {
  uint64_t now_ns = 0U;
  if (session != NULL && aula_platform_monotonic_now(&now_ns) == AULA_STATUS_OK) {
    session->status.last_activity_ns = now_ns;
  }
}

aula_status aula_media_session_request_keyframe_internal(
    aula_media_session *session) {
  aula_status status;
  if (session == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (session->state != AULA_MEDIA_SESSION_COMMITTED) {
    return AULA_STATUS_STATE_ERROR;
  }
  session->status.keyframe_requests++;
  if (session->config.backend.vtable == NULL ||
      session->config.backend.vtable->request_video_keyframe == NULL) {
    status = AULA_STATUS_UNSUPPORTED;
  } else {
    status = session->config.backend.vtable->request_video_keyframe(
        &session->config.backend);
  }
  if (status != AULA_STATUS_OK) {
    session->status.keyframe_request_failures++;
    /* Unsupported refresh is not source corruption. The initial SPS/PPS/IDR
     * gate still applies, but an established stream keeps its periodic IDRs
     * and intervening frames while the peer recovers. */
    if (status != AULA_STATUS_UNSUPPORTED) session->video_transmission_ready = 0;
  }
  return status;
}

static void media_session_destroy_stream(aula_media_session_stream *stream) {
  if (stream == NULL) return;
  aula_rtcp_reporter_destroy(stream->reporter);
  aula_rtp_send_queue_destroy(stream->dtmf_queue);
  aula_rtp_send_queue_destroy(stream->queue);
  aula_rtp_session_destroy(stream->transport);
  (void)memset(stream, 0, sizeof(*stream));
}

void aula_media_session_release_prepared(aula_media_session *session, int stop_backend) {
  if (session == NULL) return;
  aula_rx_shim_destroy(session->receive_shim);
  session->receive_shim = NULL;
  aula_aec_reset(session->aec);
  aula_aec_destroy(session->aec);
  session->aec = NULL;
  aula_dtmf_sender_destroy(session->dtmf_sender);
  session->dtmf_sender = NULL;
  aula_dtmf_receiver_destroy(session->dtmf_receiver);
  session->dtmf_receiver = NULL;
  media_session_destroy_stream(&session->audio);
  media_session_destroy_stream(&session->video);
  if (session->backend_open != 0 && session->config.backend.vtable != NULL) {
    if (stop_backend != 0 && session->backend_started != 0) {
      (void)session->config.backend.vtable->stop(&session->config.backend);
    }
    session->config.backend.vtable->close(&session->config.backend);
  }
  session->backend_open = 0;
  session->backend_started = 0;
  session->audio_remainder_length = 0U;
  session->aec_near_remainder_length = 0U;
  (void)memset(session->aec_near_remainder, 0, sizeof(session->aec_near_remainder));
  session->aec_reference_remainder_length = 0U;
  (void)memset(session->aec_reference_remainder, 0,
               sizeof(session->aec_reference_remainder));
  session->audio_sequence_number = 0U;
  session->dtmf_active_timestamp = 0U;
  session->dtmf_next_timestamp = 0U;
  session->dtmf_timestamp_valid = 0;
  session->last_dtmf_request_ns = 0U;
  session->last_receive_pli_ns = 0U;
  session->dtmf_negotiated = 0;
  session->dtmf_enabled = 0;
  session->video_transmission_ready = 0;
  session->video_transmit_enabled = 0;
  session->status.video_transmit_enabled = 0;
  session->audio_muted = 0;
  session->status.audio_muted = 0;
  (void)memset(&session->parameter_sets, 0, sizeof(session->parameter_sets));
  (void)memset(&session->receive_parameter_sets, 0, sizeof(session->receive_parameter_sets));
  aula_rx_playout_reset(&session->video.playout);
  aula_rx_playout_reset(&session->audio.playout);
  session->receive_video_waiting_for_idr = 1;
  session->status.rx_rendering = 0;
  session->status.rx_audio_rendering = 0;
  session->status.rx_video_rendering = 0;
  session->status.rx_audio_render_fresh = 0;
  session->status.rx_video_render_fresh = 0;
  session->status.rx_audio_last_success_ns = 0U;
  session->status.rx_video_last_success_ns = 0U;
  session->status.rx_renderer_healthy = 0;
  (void)memset(&session->status.aec, 0, sizeof(session->status.aec));
  (void)memset(session->negotiated_h264_profile_level_id, 0,
               sizeof(session->negotiated_h264_profile_level_id));
  (void)memset(session->status.active_h264_profile_level_id, 0,
               sizeof(session->status.active_h264_profile_level_id));
}

aula_status aula_media_session_create(const aula_media_session_config *config,
                                        aula_media_session **out_session) {
  aula_media_session *session;
  aula_status status;
  if (out_session == NULL || *out_session != NULL || !media_session_config_valid(config)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_media_backend_validate(&config->backend);
  if (status != AULA_STATUS_OK) return status;
  session = (aula_media_session *)calloc(1U, sizeof(*session));
  if (session == NULL) return AULA_STATUS_INTERNAL_ERROR;
  session->config = *config;
  session->receive_video_capacity = config->backend_config->maximum_access_unit_bytes;
  session->receive_video = (uint8_t *)malloc(session->receive_video_capacity);
  session->receive_video_render_capacity = session->receive_video_capacity + 2048U;
  session->receive_video_render = (uint8_t *)malloc(session->receive_video_render_capacity);
  session->receive_audio_capacity = (size_t)config->rtp_mtu * 24U;
  session->receive_audio = (uint8_t *)malloc(session->receive_audio_capacity);
  status = aula_h264_depacketizer_create(
      (uint32_t)session->receive_video_capacity, &session->h264_depacketizer);
  if (session->receive_video == NULL || session->receive_video_render == NULL ||
      session->receive_audio == NULL || status != AULA_STATUS_OK) {
    free(session->receive_video);
    free(session->receive_video_render);
    free(session->receive_audio);
    aula_h264_depacketizer_destroy(session->h264_depacketizer);
    free(session);
    return status == AULA_STATUS_OK ? AULA_STATUS_INTERNAL_ERROR : status;
  }
  session->state = AULA_MEDIA_SESSION_NEW;
  session->status.state = session->state;
  session->status.rx_rendering = 0;
  session->status.rx_audio_rendering = 0;
  session->status.rx_video_rendering = 0;
  session->status.rx_audio_render_fresh = 0;
  session->status.rx_video_render_fresh = 0;
  session->status.rx_renderer_healthy = 0;
  session->status.last_error = AULA_STATUS_OK;
  *out_session = session;
  return AULA_STATUS_OK;
}

aula_status aula_media_session_commit_internal(aula_media_session *session) {
  aula_status status;
  if (session == NULL || session->state != AULA_MEDIA_SESSION_PREPARED) return AULA_STATUS_STATE_ERROR;
  aula_rx_playout_init(&session->video.playout, AULA_MEDIA_SESSION_VIDEO_PACE_NS,
                        AULA_MEDIA_SESSION_RX_REORDER_HOLD_NS);
  aula_rx_playout_init(&session->audio.playout, AULA_MEDIA_SESSION_AUDIO_PACE_NS,
                        AULA_MEDIA_SESSION_RX_REORDER_HOLD_NS);
  session->receive_video_waiting_for_idr = 1;
  session->status.rx_renderer_healthy = 0;
  status = session->config.backend.vtable->start(&session->config.backend);
  if (status == AULA_STATUS_OK) session->backend_started = 1;
  if (status == AULA_STATUS_OK) status = aula_rx_shim_start(session->receive_shim);
  if (status != AULA_STATUS_OK) {
    aula_media_session_record_error(session, status);
    aula_media_session_release_prepared(session, 1);
    session->state = AULA_MEDIA_SESSION_NEW;
    session->status.state = session->state;
    return status;
  }
  session->state = AULA_MEDIA_SESSION_COMMITTED;
  session->status.state = session->state;
  session->video_transmit_enabled =
      aula_media_session_direction_sends(session->video_direction);
  session->status.video_transmit_enabled = session->video_transmit_enabled;
  session->status.audio_muted = 0;
  aula_media_session_record_activity(session);
  return AULA_STATUS_OK;
}

aula_status aula_media_session_set_video_transmit_enabled_internal(
    aula_media_session *session, int enabled) {
  if (session == NULL || (enabled != 0 && enabled != 1)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (session->state != AULA_MEDIA_SESSION_COMMITTED) return AULA_STATUS_STATE_ERROR;
  if (enabled != 0 && !aula_media_session_direction_sends(session->video_direction)) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (enabled == 0 && session->video_transmit_enabled != 0) {
    aula_status status = aula_rtp_send_queue_discard_all(session->video.queue);
    if (status != AULA_STATUS_OK) return status;
    session->video_transmission_ready = 0;
  }
  session->video_transmit_enabled = enabled;
  session->status.video_transmit_enabled = enabled;
  return AULA_STATUS_OK;
}

aula_status aula_media_session_set_audio_muted_internal(
    aula_media_session *session, int muted) {
  aula_status status;
  if (session == NULL || (muted != 0 && muted != 1)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (session->state != AULA_MEDIA_SESSION_COMMITTED) return AULA_STATUS_STATE_ERROR;
  if (muted != 0 && session->audio_muted == 0) {
    /* Audio media is queued separately from RFC 4733 events, so clearing it
     * makes the mute transition immediate without losing in-flight DTMF. */
    status = aula_rtp_send_queue_discard_all(session->audio.queue);
    if (status != AULA_STATUS_OK) return status;
    session->audio_remainder_length = 0U;
    session->aec_near_remainder_length = 0U;
    (void)memset(session->aec_near_remainder, 0, sizeof(session->aec_near_remainder));
  }
  session->audio_muted = muted;
  session->status.audio_muted = muted;
  return AULA_STATUS_OK;
}

void aula_media_session_rollback_internal(aula_media_session *session) {
  if (session == NULL || session->state == AULA_MEDIA_SESSION_COMMITTED ||
      session->state == AULA_MEDIA_SESSION_STOPPED) return;
  aula_media_session_release_prepared(session, 0);
  session->state = AULA_MEDIA_SESSION_NEW;
  session->status.state = session->state;
}

static aula_status media_session_send_bye(aula_media_session_stream *stream) {
  uint8_t output_data[16];
  aula_mutable_bytes output = {output_data, sizeof(output_data), 0U};
  aula_status status;
  if (stream == NULL || stream->reporter == NULL || stream->transport == NULL) return AULA_STATUS_OK;
  status = aula_rtcp_reporter_build_bye(stream->reporter, &output);
  if (status != AULA_STATUS_OK) return status;
  return aula_rtp_session_send_rtcp(stream->transport, (aula_bytes){output.data, output.length});
}

aula_status aula_media_session_stop_internal(aula_media_session *session) {
  aula_status result = AULA_STATUS_OK;
  aula_status status;
  if (session == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (session->state == AULA_MEDIA_SESSION_STOPPED) return AULA_STATUS_OK;
  if (session->state == AULA_MEDIA_SESSION_COMMITTED) {
    status = aula_media_session_send_receive_bye(session);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_UNSUPPORTED) result = status;
    status = media_session_send_bye(&session->audio);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_UNSUPPORTED && result == AULA_STATUS_OK) result = status;
    aula_media_session_release_prepared(session, 1);
  } else {
    aula_media_session_release_prepared(session, 0);
  }
  session->state = AULA_MEDIA_SESSION_STOPPED;
  session->status.state = session->state;
  if (result != AULA_STATUS_OK) aula_media_session_record_error(session, result);
  return result;
}

static void media_session_copy_aec_status(const aula_media_session *session,
                                          aula_media_session_status *out_status) {
  if (session->aec != NULL) (void)aula_aec_get_status(session->aec, &out_status->aec);
}

aula_status aula_media_session_get_status_internal(const aula_media_session *session,
                                            aula_media_session_status *out_status) {
  if (session == NULL || out_status == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_status = session->status;
  out_status->state = session->state;
  out_status->video_transmit_enabled = session->video_transmit_enabled;
  out_status->audio_muted = session->audio_muted;
  out_status->rx_rendering = session->status.rx_rendering;
  if (session->backend_open != 0 && session->config.backend.vtable != NULL &&
      session->config.backend.vtable->health != NULL) {
    (void)session->config.backend.vtable->health(&session->config.backend, &out_status->backend);
  }
  if (session->video.transport != NULL) (void)aula_rtp_session_get_counters(session->video.transport, &out_status->video_rtp);
  if (session->audio.transport != NULL) (void)aula_rtp_session_get_counters(session->audio.transport, &out_status->audio_rtp);
  out_status->receive.accepted_packets = session->received_packets;
  out_status->receive.rejected_packets = session->rejected_packets;
  out_status->receive.drained_bytes = session->drained_bytes;
  out_status->receive.last_packet_ns = session->status.last_activity_ns;
  out_status->receive.rx_rendering = session->status.rx_rendering;
  media_session_copy_aec_status(session, out_status);
  if (session->dtmf_receiver != NULL) {
    (void)aula_dtmf_receiver_get_stats(session->dtmf_receiver, &out_status->inbound_dtmf);
  }
  if (session->video.queue != NULL) {
    aula_rtp_send_queue_stats queue_stats;
    if (aula_rtp_send_queue_get_stats(session->video.queue, &queue_stats) == AULA_STATUS_OK) {
      out_status->video_packets_queued = queue_stats.queued_packets;
      out_status->dropped_video_packets = queue_stats.dropped_packets +
                                          session->video.send_drop_packets;
    }
  }
  if (session->audio.queue != NULL) {
    aula_rtp_send_queue_stats queue_stats;
    if (aula_rtp_send_queue_get_stats(session->audio.queue, &queue_stats) == AULA_STATUS_OK) {
      out_status->audio_packets_queued = queue_stats.queued_packets;
      out_status->dropped_audio_packets = queue_stats.dropped_packets +
                                          session->audio.send_drop_packets;
    }
  }
  if (session->audio.dtmf_queue != NULL) {
    aula_rtp_send_queue_stats queue_stats;
    if (aula_rtp_send_queue_get_stats(session->audio.dtmf_queue, &queue_stats) ==
        AULA_STATUS_OK) {
      out_status->audio_packets_queued += queue_stats.queued_packets;
      out_status->dropped_audio_packets += queue_stats.dropped_packets;
    }
  }
  out_status->video_packets_sent = session->video.sent_packets;
  out_status->audio_packets_sent = session->audio.sent_packets;
  return AULA_STATUS_OK;
}

aula_status aula_media_session_get_metrics_internal(
    const aula_media_session *session, aula_media_metrics *out_metrics) {
  aula_media_session_status status;
  if (session == NULL || out_metrics == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_metrics, 0, sizeof(*out_metrics));
  if (aula_media_session_get_status_internal(session, &status) !=
      AULA_STATUS_OK)
    return AULA_STATUS_STATE_ERROR;
  out_metrics->video_queue_depth = status.video_packets_queued;
  out_metrics->audio_queue_depth = status.audio_packets_queued;
  out_metrics->video_packet_drops = status.dropped_video_packets;
  out_metrics->audio_packet_drops = status.dropped_audio_packets;
  out_metrics->video_access_unit_drops = session->video_access_unit_drops;
  out_metrics->backend_drops = status.backend.frames_dropped;
  out_metrics->backend_restarts = status.backend.restarts;
  return AULA_STATUS_OK;
}

size_t aula_media_session_get_descriptors_internal(
    const aula_media_session *session, int *descriptors, size_t capacity) {
  size_t count;
  if (session == NULL || descriptors == NULL || capacity == 0U ||
      session->state != AULA_MEDIA_SESSION_COMMITTED)
    return 0U;
  count = aula_rtp_session_get_descriptors_internal(
      session->video.transport, descriptors, capacity);
  if (count < capacity)
    count += aula_rtp_session_get_descriptors_internal(
        session->audio.transport, descriptors + count, capacity - count);
  return count;
}

void aula_media_session_destroy_internal(aula_media_session *session) {
  if (session == NULL) return;
  (void)aula_media_session_stop(session);
  free(session->receive_video);
  free(session->receive_video_render);
  free(session->receive_audio);
  aula_h264_depacketizer_destroy(session->h264_depacketizer);
  (void)memset(session, 0, sizeof(*session));
  free(session);
}

/* The caller has authenticated and validated this compound. Bound log volume
 * independently of its sender; emit only report counters and a target match. */
void aula_media_session_report_rtcp_internal(
    const aula_media_session *session, const aula_media_session_stream *stream,
    aula_bytes packet) {
  aula_rtcp_frame frame;
  size_t offset = 0U;
  unsigned int reports = 0U;
  if (session->received_packets >= 4U) return;
  while (reports < 2U &&
         aula_rtcp_frame_next(packet, &offset, &frame) == AULA_STATUS_OK) {
    const uint8_t *block;
    size_t header;
    uint32_t target = 0U, highest = 0U, jitter = 0U, loss = 0U;
    int32_t signed_loss = 0;
    unsigned int fraction = 0U;
    if (frame.packet_type != AULA_RTCP_RR &&
        frame.packet_type != AULA_RTCP_SR) continue;
    header = frame.packet_type == AULA_RTCP_RR ? 8U : 28U;
    if (frame.count != 0U && frame.content_length >= header + 24U) {
      block = frame.data + header;
      target = aula_rtcp_frame_read_u32(block);
      fraction = block[4];
      loss = aula_rtcp_frame_read_u32(block + 4U) & UINT32_C(0x00ffffff);
      signed_loss = (int32_t)loss;
      if ((loss & UINT32_C(0x00800000)) != 0U) signed_loss -= INT32_C(0x01000000);
      highest = aula_rtcp_frame_read_u32(block + 8U);
      jitter = aula_rtcp_frame_read_u32(block + 12U);
    }
    (void)fprintf(stderr,
        "aula-sipd: rtcp-report video=%d type=%u blocks=%u local_target=%d "
        "highest_sequence=%lu fraction_lost=%u cumulative_loss=%ld jitter=%lu\n",
        stream == &session->video, (unsigned int)frame.packet_type,
        (unsigned int)frame.count, frame.count != 0U && target == stream->identity.ssrc,
        (unsigned long)highest, fraction, (long)signed_loss, (unsigned long)jitter);
    ++reports;
  }
}
