#include "session_private.h"
#include "aula_sipd/platform.h"

static aula_status media_session_poll_result(aula_status status, int accept_end,
                                              aula_status *result) {
  if (status == AULA_STATUS_OK) {
    *result = AULA_STATUS_OK;
    return AULA_STATUS_OK;
  }
  if (status == AULA_STATUS_AGAIN || (accept_end != 0 && status == AULA_STATUS_END)) {
    return AULA_STATUS_OK;
  }
  return status;
}

static int media_session_direction_receives(aula_sdp_direction direction) {
  return direction == AULA_SDP_SENDRECV || direction == AULA_SDP_RECVONLY;
}

static int media_session_stream_receives(const aula_media_session *session,
                                         const aula_media_session_stream *stream) {
  return stream == &session->video ?
      media_session_direction_receives(session->video_direction) :
      media_session_direction_receives(session->audio_direction);
}

static int media_session_stream_active(const aula_media_session *session,
                                       const aula_media_session_stream *stream) {
  return stream == &session->video ? session->video_direction != AULA_SDP_INACTIVE :
      session->audio_direction != AULA_SDP_INACTIVE;
}

static uint64_t media_session_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void media_session_consider_queue(const aula_rtp_send_queue *queue,
                                         uint64_t *deadline_ns) {
  aula_rtp_send_queue_stats stats;
  if (queue != NULL &&
      aula_rtp_send_queue_get_stats(queue, &stats) == AULA_STATUS_OK &&
      stats.queued_packets != 0U && stats.next_send_ns < *deadline_ns) {
    *deadline_ns = stats.next_send_ns;
  }
}

uint64_t aula_media_session_next_action_ns(const aula_media_session *session,
                                             uint64_t now_ns) {
  uint64_t deadline_ns = UINT64_MAX;
  uint64_t playout_ns;
  if (session == NULL || session->state != AULA_MEDIA_SESSION_COMMITTED)
    return deadline_ns;
  if (session->video_direction != AULA_SDP_INACTIVE) {
    deadline_ns = media_session_saturating_add(
        now_ns, AULA_MEDIA_SESSION_VIDEO_PACE_NS);
  }
  media_session_consider_queue(session->video.queue, &deadline_ns);
  media_session_consider_queue(session->audio.dtmf_queue, &deadline_ns);
  media_session_consider_queue(session->audio.queue, &deadline_ns);
  if (media_session_stream_receives(session, &session->video)) {
    playout_ns = aula_rx_playout_next_action_ns(&session->video.playout, now_ns);
    if (playout_ns < deadline_ns) deadline_ns = playout_ns;
  }
  if (media_session_stream_receives(session, &session->audio)) {
    playout_ns = aula_rx_playout_next_action_ns(&session->audio.playout, now_ns);
    if (playout_ns < deadline_ns) deadline_ns = playout_ns;
  }
  return deadline_ns;
}

static aula_status media_session_drain_pair(aula_media_session *session,
                                             aula_status *result) {
  aula_status status = AULA_STATUS_AGAIN;
  if (media_session_stream_active(session, &session->video)) {
    status = aula_media_session_drain_stream_internal(session, &session->video);
  }
  status = media_session_poll_result(status, 0, result);
  if (status != AULA_STATUS_OK) return status;
  status = media_session_stream_active(session, &session->audio) ?
      aula_media_session_drain_stream_internal(session, &session->audio) :
      AULA_STATUS_AGAIN;
  return media_session_poll_result(status, 0, result);
}

static aula_status media_session_report_pair(aula_media_session *session,
                                              aula_status *result) {
  aula_status status = media_session_stream_receives(session, &session->video) ?
      aula_media_session_emit_receive_report_internal(session, &session->video) :
      AULA_STATUS_AGAIN;
  status = media_session_poll_result(status, 0, result);
  if (status != AULA_STATUS_OK) return status;
  status = media_session_stream_receives(session, &session->audio) ?
      aula_media_session_emit_receive_report_internal(session, &session->audio) :
      AULA_STATUS_AGAIN;
  return media_session_poll_result(status, 0, result);
}

static aula_status media_session_playout_pair(aula_media_session *session,
                                               aula_status *result) {
  aula_status status = media_session_stream_receives(session, &session->video) ?
      media_session_poll_result(
          aula_media_session_playout_stream_internal(session, &session->video), 0, result) :
      AULA_STATUS_OK;
  if (status != AULA_STATUS_OK) return status;
  return media_session_stream_receives(session, &session->audio) ?
      media_session_poll_result(
          aula_media_session_playout_stream_internal(session, &session->audio), 0, result) :
      AULA_STATUS_OK;
}

static aula_status media_session_pump_pair(aula_media_session *session,
                                            aula_deadline deadline,
                                            aula_status *result) {
  aula_status status = media_session_poll_result(
      aula_media_session_pump_video_internal(session, deadline), 1, result);
  if (status != AULA_STATUS_OK) return status;
  return media_session_poll_result(
      aula_media_session_pump_audio_internal(session, deadline), 1, result);
}

static aula_status media_session_flush_queue(aula_media_session *session,
                                              aula_media_session_stream *stream,
                                              aula_rtp_send_queue *queue,
                                              aula_status *result) {
  aula_rtp_send_queue *saved_queue;
  aula_status status;
  unsigned int index;
  unsigned int budget = stream == &session->video ? 4U :
      (queue == stream->dtmf_queue ? 1U : 2U);
  if (queue == NULL) return AULA_STATUS_OK;
  saved_queue = stream->queue;
  stream->queue = queue;
  status = AULA_STATUS_AGAIN;
  for (index = 0U; index < budget; ++index) {
    status = aula_media_session_flush_stream_internal(session, stream);
    if (status != AULA_STATUS_OK) break;
    *result = AULA_STATUS_OK;
  }
  stream->queue = saved_queue;
  return media_session_poll_result(status, 0, result);
}

static aula_status media_session_flush_audio(aula_media_session *session,
                                              aula_status *result) {
  if (!aula_media_session_direction_sends(session->audio_direction)) {
    return AULA_STATUS_OK;
  }
  aula_status status = media_session_flush_queue(session, &session->audio,
      session->audio.dtmf_queue, result);
  if (status != AULA_STATUS_OK || session->audio_muted != 0) return status;
  return media_session_flush_queue(session, &session->audio, session->audio.queue,
                                   result);
}

static aula_status media_session_flush_pair(aula_media_session *session,
                                             aula_status *result) {
  aula_status status = AULA_STATUS_OK;
  if (session->video_transmit_enabled != 0) {
    status = media_session_flush_queue(session, &session->video,
        session->video.queue, result);
  }
  if (status != AULA_STATUS_OK) return status;
  status = aula_media_session_flush_repairs_internal(session);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  return media_session_flush_audio(session, result);
}

aula_status aula_media_session_poll_internal(aula_media_session *session,
                                               aula_deadline deadline) {
  aula_status result = AULA_STATUS_AGAIN;
  aula_status status;
  if (session == NULL || session->state != AULA_MEDIA_SESSION_COMMITTED) {
    return AULA_STATUS_STATE_ERROR;
  }
  status = media_session_drain_pair(session, &result);
  if (status == AULA_STATUS_OK) status = media_session_playout_pair(session, &result);
  if (status == AULA_STATUS_OK) status = media_session_report_pair(session, &result);
  if (status == AULA_STATUS_OK) status = media_session_pump_pair(session, deadline, &result);
  if (status == AULA_STATUS_OK) status = media_session_flush_pair(session, &result);
  if (status == AULA_STATUS_OK) return result;
  aula_media_session_record_error(session, status);
  return status;
}

static int media_session_dtmf_state_valid(const aula_media_session *session) {
  return session != NULL && session->state == AULA_MEDIA_SESSION_COMMITTED &&
      session->dtmf_enabled != 0 && session->dtmf_sender != NULL &&
      aula_media_session_direction_sends(session->audio_direction);
}

static int media_session_dtmf_rate_limited(const aula_media_session *session,
                                           uint64_t now_ns) {
  return session->last_dtmf_request_ns != 0U &&
      now_ns >= session->last_dtmf_request_ns &&
      now_ns - session->last_dtmf_request_ns <
          AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS;
}

static uint32_t media_session_dtmf_start_timestamp(
    const aula_media_session *session) {
  uint32_t audio_timestamp = session->g711_packetizer.rtp_timestamp;
  uint32_t distance;
  if (session->dtmf_timestamp_valid == 0) return audio_timestamp;
  distance = audio_timestamp - session->dtmf_next_timestamp;
  return distance < (UINT32_MAX / 2U + 1U)
      ? audio_timestamp : session->dtmf_next_timestamp;
}

static void media_session_advance_dtmf_timestamp(aula_media_session *session,
                                                 uint16_t duration_samples) {
  session->dtmf_next_timestamp =
      session->dtmf_active_timestamp + (uint32_t)duration_samples;
}

aula_status aula_media_session_send_dtmf_internal(aula_media_session *session,
                                                     uint8_t digit,
                                                     uint16_t duration_samples,
                                                     int end) {
  uint8_t wire[16];
  aula_mutable_bytes output = {wire, sizeof(wire), 0U};
  aula_rtp_packet packet;
  aula_dtmf_event active;
  uint64_t now_ns = 0U;
  aula_status status;
  if (!media_session_dtmf_state_valid(session)) return AULA_STATUS_STATE_ERROR;
  status = aula_platform_monotonic_now(&now_ns);
  if (status != AULA_STATUS_OK) return status;
  if (media_session_dtmf_rate_limited(session, now_ns)) {
    session->status.rejected_dtmf_requests++;
    return AULA_STATUS_AGAIN;
  }
  status = aula_dtmf_sender_get_active(session->dtmf_sender, &active);
  if (status == AULA_STATUS_END) {
    uint32_t timestamp = media_session_dtmf_start_timestamp(session);
    status = aula_dtmf_sender_begin(session->dtmf_sender, digit,
                                     timestamp);
    if (status == AULA_STATUS_OK) {
      session->dtmf_active_timestamp = timestamp;
      session->dtmf_timestamp_valid = 1;
    }
  } else if (status == AULA_STATUS_OK && active.digit != digit) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (status != AULA_STATUS_OK) return status;
  status = aula_dtmf_sender_emit(session->dtmf_sender, duration_samples, end, &output);
  if (status != AULA_STATUS_OK) return status;
  media_session_advance_dtmf_timestamp(session, duration_samples);
  status = aula_rtp_parse((aula_bytes){output.data, output.length}, &packet);
  if (status != AULA_STATUS_OK) return status;
  packet.header.sequence_number = session->audio_sequence_number++;
  status = aula_rtp_send_queue_enqueue(session->audio.dtmf_queue, &packet, now_ns);
  if (status == AULA_STATUS_LIMIT_EXCEEDED) session->audio.send_drop_packets++;
  if (status == AULA_STATUS_OK) {
    session->last_dtmf_request_ns = now_ns;
  }
  return status;
}
