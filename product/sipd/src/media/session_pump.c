#include "session_private.h"
#include "aula_sipd/h264.h"
#include "aula_sipd/platform.h"
#include "aula_sipd/rtcp.h"
#include "../rtcp/feedback.h"
#include "../rtp/pacing_internal.h"
#include <string.h>

typedef struct media_session_h264_callback_context {
  aula_rtp_send_batch *batch;
  int final_nal;
} media_session_h264_callback_context;

static aula_status media_session_h264_packet_callback(void *context,
                                                       const aula_rtp_packet *packet) {
  media_session_h264_callback_context *callback_context =
      (media_session_h264_callback_context *)context;
  aula_rtp_packet adjusted;
  if (callback_context == NULL || callback_context->batch == NULL || packet == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  adjusted = *packet;
  adjusted.header.marker = (uint8_t)(callback_context->final_nal != 0 && packet->header.marker != 0);
  return aula_rtp_send_batch_stage(callback_context->batch, &adjusted);
}

static uint32_t media_session_video_timestamp(uint64_t pts_ns) {
  uint64_t ticks = (pts_ns / UINT64_C(1000000)) * UINT64_C(90) +
                   ((pts_ns % UINT64_C(1000000)) * UINT64_C(90)) / UINT64_C(1000000);
  return (uint32_t)ticks;
}

static aula_status media_session_read_video_frame(aula_media_session *session,
                                                   aula_deadline deadline,
                                                   aula_media_frame *frame) {
  aula_status status;
  (void)memset(frame, 0, sizeof(*frame));
  status = session->config.backend.vtable->read_video_access_unit(
      &session->config.backend, deadline, frame);
  if (status != AULA_STATUS_OK) return status;
  if (aula_media_validate_frame(frame) != AULA_STATUS_OK ||
      frame->kind != AULA_MEDIA_VIDEO_H264_ANNEX_B) {
    return AULA_STATUS_INVALID_DATA;
  }
  return AULA_STATUS_OK;
}

static aula_status media_session_update_h264_parameters(aula_media_session *session,
                                                         const aula_media_frame *frame,
                                                         int *out_has_idr) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  aula_status status;
  if (out_has_idr == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_has_idr = 0;
  status = aula_h264_annexb_iterator_init(&iterator, frame->data,
                                           session->config.backend_config->maximum_access_unit_bytes);
  while (status == AULA_STATUS_OK) {
    status = aula_h264_annexb_iterator_next(&iterator, &nal);
    if (status == AULA_STATUS_OK) {
      if (nal.is_idr != 0) *out_has_idr = 1;
      status = aula_h264_parameter_sets_update(&session->parameter_sets, &nal);
      if (status != AULA_STATUS_OK) return status;
      if (nal.type == 7U) {
        status = aula_h264_profile_level_id(&session->parameter_sets,
                                             session->status.active_h264_profile_level_id);
        if (status != AULA_STATUS_OK) return status;
        if (!aula_media_session_h264_profile_matches(session)) {
          session->video_transmission_ready = 0;
          return AULA_STATUS_INVALID_DATA;
        }
      }
    }
  }
  if (status != AULA_STATUS_END) return status;
  return AULA_STATUS_OK;
}

static aula_status media_session_require_video_ready(aula_media_session *session,
                                                      int has_idr) {
  if (session->video_transmission_ready == 0) {
    if (!aula_h264_can_transmit_access_unit(&session->parameter_sets, has_idr)) {
      (void)aula_media_session_request_keyframe_internal(session);
      return AULA_STATUS_AGAIN;
    }
    session->video_transmission_ready = 1;
  }
  return AULA_STATUS_OK;
}

static aula_status media_session_packetize_h264_nal(
    aula_media_session *session, const aula_h264_nal *nal,
    const aula_h264_annexb_iterator *next_iterator,
    aula_rtp_send_batch *batch) {
  media_session_h264_callback_context callback_context;
  aula_h264_annexb_iterator lookahead = *next_iterator;
  aula_h264_nal ignored;
  aula_status next_status = aula_h264_annexb_iterator_next(&lookahead, &ignored);
  aula_status status;
  if (next_status != AULA_STATUS_OK && next_status != AULA_STATUS_END) return next_status;
  callback_context.batch = batch;
  callback_context.final_nal = next_status == AULA_STATUS_END;
  if (nal->length <= (size_t)session->h264_packetizer.mtu - 12U) {
    status = aula_h264_packetize_single_nal(&session->h264_packetizer, nal,
                                              media_session_h264_packet_callback,
                                              &callback_context);
  } else {
    status = aula_h264_packetize_fu_a(&session->h264_packetizer, nal,
                                       media_session_h264_packet_callback,
                                       &callback_context);
  }
  return status;
}

static aula_status media_session_h264_batch_size(
    const aula_media_session *session, const aula_media_frame *frame,
    uint32_t *out_packets, uint32_t *out_bytes) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  size_t single_limit = (size_t)session->h264_packetizer.mtu - 12U;
  size_t fragment_limit = (size_t)session->h264_packetizer.mtu - 14U;
  uint64_t packets = 0U;
  uint64_t bytes = 0U;
  aula_status status = aula_h264_annexb_iterator_init(
      &iterator, frame->data,
      session->config.backend_config->maximum_access_unit_bytes);
  if (status != AULA_STATUS_OK) return status;
  while ((status = aula_h264_annexb_iterator_next(&iterator, &nal)) ==
         AULA_STATUS_OK) {
    uint64_t nal_packets = 1U;
    uint64_t nal_bytes = nal.length;
    if (nal.length > single_limit) {
      uint64_t body = nal.length - 1U;
      nal_packets = (body + fragment_limit - 1U) / fragment_limit;
      nal_bytes = body + nal_packets * 2U;
    }
    packets += nal_packets;
    bytes += nal_bytes;
    if (packets > UINT32_MAX || bytes > UINT32_MAX)
      return AULA_STATUS_LIMIT_EXCEEDED;
  }
  if (status != AULA_STATUS_END || packets == 0U)
    return status == AULA_STATUS_END ? AULA_STATUS_INVALID_DATA : status;
  *out_packets = (uint32_t)packets;
  *out_bytes = (uint32_t)bytes;
  return AULA_STATUS_OK;
}

static void media_session_record_access_unit_drop(aula_media_session *session) {
  if (session->video_access_unit_drops != UINT64_MAX)
    ++session->video_access_unit_drops;
}

static aula_status media_session_packetize_video_frame(aula_media_session *session,
                                                         const aula_media_frame *frame) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  aula_rtp_send_batch *batch = NULL;
  uint32_t packet_count = 0U;
  uint32_t payload_bytes = 0U;
  uint16_t saved_sequence;
  uint64_t now_ns = 0U;
  aula_status status = media_session_h264_batch_size(
      session, frame, &packet_count, &payload_bytes);
  if (status != AULA_STATUS_OK) {
    media_session_record_access_unit_drop(session);
    return status;
  }
  (void)aula_platform_monotonic_now(&now_ns);
  status = aula_rtp_send_queue_batch_begin(
      session->video.queue, packet_count, payload_bytes, now_ns, &batch);
  if (status != AULA_STATUS_OK) {
    media_session_record_access_unit_drop(session);
    return status;
  }
  status = aula_h264_annexb_iterator_init(&iterator, frame->data,
      session->config.backend_config->maximum_access_unit_bytes);
  saved_sequence = session->h264_packetizer.sequence_number;
  session->h264_packetizer.timestamp = media_session_video_timestamp(frame->pts_ns);
  while ((status = aula_h264_annexb_iterator_next(&iterator, &nal)) == AULA_STATUS_OK) {
    status = media_session_packetize_h264_nal(session, &nal, &iterator, batch);
    if (status != AULA_STATUS_OK) break;
  }
  if (status == AULA_STATUS_END) status = aula_rtp_send_batch_commit(batch);
  if (status == AULA_STATUS_OK) return AULA_STATUS_OK;
  session->h264_packetizer.sequence_number = saved_sequence;
  aula_rtp_send_batch_abort(batch);
  media_session_record_access_unit_drop(session);
  return status;
}

aula_status aula_media_session_pump_video_internal(aula_media_session *session,
                                                      aula_deadline deadline) {
  aula_media_frame frame;
  int has_idr;
  int readiness_before_admission;
  aula_status status;
  if (!aula_media_session_direction_sends(session->video_direction) ||
      session->video_transmit_enabled == 0) return AULA_STATUS_AGAIN;
  status = media_session_read_video_frame(session, deadline, &frame);
  if (status == AULA_STATUS_OK) status = media_session_update_h264_parameters(session, &frame, &has_idr);
  if (status == AULA_STATUS_OK) {
    readiness_before_admission = session->video_transmission_ready;
    status = media_session_require_video_ready(session, has_idr);
  }
  if (status == AULA_STATUS_OK) {
    status = media_session_packetize_video_frame(session, &frame);
    if (status != AULA_STATUS_OK)
      session->video_transmission_ready = readiness_before_admission;
    if (status == AULA_STATUS_LIMIT_EXCEEDED) status = AULA_STATUS_OK;
  }
  return status;
}

aula_status aula_media_session_flush_stream_internal(
    aula_media_session *session, aula_media_session_stream *stream) {
  aula_rtp_packet packet;
  aula_rtcp_sender_metrics metrics;
  uint8_t report_data[256];
  aula_mutable_bytes report = {report_data, sizeof(report_data), 0U};
  uint64_t now_ns = 0U;
  uint64_t ntp_timestamp = 0U;
  aula_status status;
  (void)aula_platform_monotonic_now(&now_ns);
  status = aula_rtp_send_queue_dequeue_bounded(stream->queue, now_ns,
      stream == &session->video ? 4U :
      (stream->queue == stream->dtmf_queue ? 1U : 2U), &packet);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_session_send_rtp(stream->transport, &packet);
  if (status != AULA_STATUS_OK) {
    /* Dequeue transfers queue ownership; current transport has no requeue
     * primitive, so a would-block send is an explicit bounded drop. */
    stream->send_drop_packets++;
    return status;
  }
  stream->sent_packets++;
  stream->sent_octets += packet.payload.length;
  status = aula_platform_ntp_now(&ntp_timestamp);
  if (status != AULA_STATUS_OK) return status;
  (void)memset(&metrics, 0, sizeof(metrics));
  metrics.ssrc = stream->identity.ssrc;
  metrics.ntp_timestamp = ntp_timestamp;
  metrics.rtp_timestamp = packet.header.timestamp;
  metrics.packet_count = stream->sent_packets > UINT32_MAX ? UINT32_MAX :
                         (uint32_t)stream->sent_packets;
  metrics.octet_count = stream->sent_octets > UINT32_MAX ? UINT32_MAX :
                        (uint32_t)stream->sent_octets;
  status = aula_rtcp_reporter_build_sender_report(stream->reporter, &metrics, &report);
  if (status == AULA_STATUS_OK) status = aula_rtp_session_send_rtcp(
      stream->transport, (aula_bytes){report.data, report.length});
  if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  aula_media_session_record_activity(session);
  return AULA_STATUS_OK;
}

static int media_session_is_untrusted_datagram_status(aula_status status) {
  return status == AULA_STATUS_INVALID_DATA || status == AULA_STATUS_LIMIT_EXCEEDED ||
      status == AULA_STATUS_PERMISSION_DENIED || status == AULA_STATUS_SECURITY_ERROR ||
      status == AULA_STATUS_UNSUPPORTED || status == AULA_STATUS_STATE_ERROR;
}

static int media_session_reject_untrusted(aula_media_session *session, aula_status status) {
  if (!media_session_is_untrusted_datagram_status(status)) return 0;
  session->rejected_packets++;
  return 1;
}

static aula_status media_session_queue_received_packet(
    aula_media_session_stream *stream, const aula_rtp_packet *packet) {
  uint64_t now_ns = 0U;
  aula_status status = aula_platform_monotonic_now(&now_ns);
  if (status != AULA_STATUS_OK) return status;
  return aula_rx_playout_push(&stream->playout, packet, now_ns);
}

static uint8_t media_session_dtmf_receive_payload(
    const aula_media_session *session) {
  return session->directional_payloads_configured != 0 ?
      session->dtmf_receive_payload_type : session->dtmf_payload_type;
}

static aula_status media_session_drain_rtp(aula_media_session *session,
                                            aula_media_session_stream *stream,
                                            const aula_rx_media_profile *receive_profile,
                                            aula_mutable_bytes *packet_buffer,
                                            int *out_continue) {
  aula_rtp_source source;
  aula_rtp_packet packet;
  aula_status status;
  packet_buffer->length = 0U;
  status = aula_rtp_session_receive_rtp(stream->transport, packet_buffer, &source, &packet);
  if (status != AULA_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return AULA_STATUS_OK;
    }
    return status;
  }
  status = aula_rx_shim_accept_rtp_for_media(session->receive_shim, receive_profile, &source,
                                              (aula_bytes){packet_buffer->data,
                                                            packet_buffer->length});
  if (status != AULA_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return AULA_STATUS_OK;
    }
    return status;
  }
  stream->received_ssrc = source.ssrc;
  if (stream == &session->audio && session->dtmf_enabled != 0 &&
      packet.header.payload_type == media_session_dtmf_receive_payload(session)) {
    aula_dtmf_event ignored_event;
    status = aula_dtmf_receiver_accept(session->dtmf_receiver, &packet, &ignored_event);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) {
      if (media_session_reject_untrusted(session, status)) {
        *out_continue = 1;
        return AULA_STATUS_OK;
      }
      return status;
    }
  } else {
    status = media_session_queue_received_packet(stream, &packet);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN &&
        status != AULA_STATUS_LIMIT_EXCEEDED) {
      if (media_session_reject_untrusted(session, status)) {
        *out_continue = 1;
        return AULA_STATUS_OK;
      }
      return status;
    }
  }
  session->received_packets++;
  session->drained_bytes += packet_buffer->length;
  return AULA_STATUS_OK;
}

static aula_status media_session_drain_rtcp(aula_media_session *session,
                                             aula_media_session_stream *stream,
                                             aula_mutable_bytes *packet_buffer,
                                             int receive_rtp,
                                             int *out_continue) {
  aula_rtp_source source;
  aula_rtcp_compound_summary summary;
  aula_status status;
  packet_buffer->length = 0U;
  status = aula_rtp_session_receive_rtcp(stream->transport, packet_buffer, &source);
  if (status != AULA_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return AULA_STATUS_OK;
    }
    return status;
  }
  if (aula_rtcp_visit_feedback(
          (aula_bytes){packet_buffer->data, packet_buffer->length}, NULL, NULL) !=
      AULA_STATUS_OK) {
    session->rejected_packets++;
    *out_continue = 1;
    return AULA_STATUS_OK;
  }
  source.ssrc = ((uint32_t)packet_buffer->data[4] << 24) |
                ((uint32_t)packet_buffer->data[5] << 16) |
                ((uint32_t)packet_buffer->data[6] << 8) | packet_buffer->data[7];
  if (receive_rtp != 0) {
    status = aula_rx_shim_accept_rtcp_source_internal(
        session->receive_shim, stream->receive_media_id, &source,
        (aula_bytes){packet_buffer->data, packet_buffer->length}, &summary);
  } else {
    status = aula_rtcp_parse_compound(
        (aula_bytes){packet_buffer->data, packet_buffer->length}, &summary);
  }
  if (status == AULA_STATUS_OK)
    status = aula_media_session_accept_feedback_internal(session, stream, source.ssrc,
        (aula_bytes){packet_buffer->data, packet_buffer->length});
  if (status == AULA_STATUS_AGAIN) {
    *out_continue = 1;
    return AULA_STATUS_OK;
  }
  if (status != AULA_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return AULA_STATUS_OK;
    }
    return status;
  }
  if (receive_rtp == 0) aula_media_session_report_rtcp_internal(
      session, stream, (aula_bytes){packet_buffer->data, packet_buffer->length});
  session->received_packets++;
  session->drained_bytes += packet_buffer->length;
  return AULA_STATUS_OK;
}

static int media_session_stream_receives_rtp(
    const aula_media_session *session, const aula_media_session_stream *stream) {
  aula_sdp_direction direction = stream == &session->video ?
      session->video_direction : session->audio_direction;
  return direction == AULA_SDP_SENDRECV || direction == AULA_SDP_RECVONLY;
}

static aula_status media_session_discard_rtp(
    aula_media_session *session, aula_media_session_stream *stream,
    aula_mutable_bytes *packet_buffer, int *out_continue) {
  aula_rtp_source source;
  aula_rtp_packet packet;
  aula_status status;
  packet_buffer->length = 0U;
  status = aula_rtp_session_receive_rtp(stream->transport, packet_buffer,
                                         &source, &packet);
  if (status != AULA_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return AULA_STATUS_OK;
    }
    return status;
  }
  session->rejected_packets++;
  session->drained_bytes += packet_buffer->length;
  *out_continue = 1;
  return AULA_STATUS_OK;
}

aula_status aula_media_session_drain_stream_internal(
    aula_media_session *session, aula_media_session_stream *stream) {
  uint8_t packet_storage[AULA_SIPD_MAX_RTP_PACKET_BYTES];
  aula_mutable_bytes packet_buffer = {packet_storage, session->config.rtp_mtu, 0U};
  aula_rx_media_profile receive_profile;
  int receive_rtp;
  unsigned int count;
  receive_profile.media_id = stream->receive_media_id;
  receive_profile.payload_clocks = stream->payload_clocks;
  receive_profile.payload_clock_count = stream->payload_type_count;
  receive_rtp = media_session_stream_receives_rtp(session, stream);
  for (count = 0U; count < AULA_MEDIA_SESSION_MAX_INGRESS_PER_POLL; ++count) {
    int rtp_ready = 0;
    int rtcp_ready = 0;
    int continue_drain = 0;
    aula_status status = aula_rtp_session_poll(stream->transport, &rtp_ready, &rtcp_ready);
    if (status == AULA_STATUS_AGAIN) return count == 0U ? AULA_STATUS_AGAIN : AULA_STATUS_OK;
    if (status != AULA_STATUS_OK) return status;
    if (rtcp_ready != 0 && receive_rtp == 0) {
      status = media_session_drain_rtcp(session, stream, &packet_buffer,
                                        receive_rtp, &continue_drain);
    } else if (rtp_ready != 0 && receive_rtp != 0) {
      status = media_session_drain_rtp(session, stream, &receive_profile, &packet_buffer,
                                       &continue_drain);
    } else if (rtp_ready != 0) {
      status = media_session_discard_rtp(session, stream, &packet_buffer,
                                         &continue_drain);
    } else if (rtcp_ready != 0) {
      status = media_session_drain_rtcp(session, stream, &packet_buffer,
                                        receive_rtp, &continue_drain);
    }
    if (status != AULA_STATUS_OK) return status;
    if (continue_drain != 0) continue;
    aula_media_session_record_activity(session);
  }
  return AULA_STATUS_OK;
}

aula_status aula_media_session_emit_receive_report_internal(
    aula_media_session *session, aula_media_session_stream *stream) {
  uint8_t report_data[256];
  aula_mutable_bytes report = {report_data, sizeof(report_data), 0U};
  aula_rtcp_report_metrics metrics;
  aula_status status;
  if (session == NULL || stream == NULL || session->receive_shim == NULL ||
      stream->transport == NULL || stream->reporter == NULL || stream->received_ssrc == 0U) {
    return AULA_STATUS_AGAIN;
  }
  status = aula_rx_shim_get_receiver_report_metrics_for_media(
      session->receive_shim, stream->receive_media_id, stream->received_ssrc,
      stream->identity.ssrc, &metrics);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtcp_reporter_build_receiver_report(stream->reporter, &metrics, &report);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_session_send_rtcp(stream->transport,
                                       (aula_bytes){report.data, report.length});
  if (status != AULA_STATUS_OK) return status;
  return aula_rx_shim_commit_receiver_report_for_media(session->receive_shim,
                                                         stream->receive_media_id,
                                                         stream->received_ssrc);
}

aula_status aula_media_session_send_receive_bye(aula_media_session *session) {
  uint8_t output_data[16];
  aula_mutable_bytes output = {output_data, sizeof(output_data), 0U};
  aula_status status;
  if (session == NULL || session->receive_shim == NULL || session->video.transport == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_rx_shim_stop(session->receive_shim, &output);
  if (status != AULA_STATUS_OK) return status;
  return aula_rtp_session_send_rtcp(session->video.transport,
                                     (aula_bytes){output.data, output.length});
}
