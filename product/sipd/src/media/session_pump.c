#include "session_private.h"
#include "ls200_sipd/h264.h"
#include "ls200_sipd/platform.h"
#include "ls200_sipd/rtcp.h"
#include "../rtcp/feedback.h"
#include "../rtp/pacing_internal.h"
#include <string.h>

typedef struct media_session_h264_callback_context {
  ls200_rtp_send_batch *batch;
  int final_nal;
} media_session_h264_callback_context;

static ls200_status media_session_h264_packet_callback(void *context,
                                                       const ls200_rtp_packet *packet) {
  media_session_h264_callback_context *callback_context =
      (media_session_h264_callback_context *)context;
  ls200_rtp_packet adjusted;
  if (callback_context == NULL || callback_context->batch == NULL || packet == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  adjusted = *packet;
  adjusted.header.marker = (uint8_t)(callback_context->final_nal != 0 && packet->header.marker != 0);
  return ls200_rtp_send_batch_stage(callback_context->batch, &adjusted);
}

static uint32_t media_session_video_timestamp(uint64_t pts_ns) {
  uint64_t ticks = (pts_ns / UINT64_C(1000000)) * UINT64_C(90) +
                   ((pts_ns % UINT64_C(1000000)) * UINT64_C(90)) / UINT64_C(1000000);
  return (uint32_t)ticks;
}

static ls200_status media_session_read_video_frame(ls200_media_session *session,
                                                   ls200_deadline deadline,
                                                   ls200_media_frame *frame) {
  ls200_status status;
  (void)memset(frame, 0, sizeof(*frame));
  status = session->config.backend.vtable->read_video_access_unit(
      &session->config.backend, deadline, frame);
  if (status != LS200_STATUS_OK) return status;
  if (ls200_media_validate_frame(frame) != LS200_STATUS_OK ||
      frame->kind != LS200_MEDIA_VIDEO_H264_ANNEX_B) {
    return LS200_STATUS_INVALID_DATA;
  }
  return LS200_STATUS_OK;
}

static ls200_status media_session_update_h264_parameters(ls200_media_session *session,
                                                         const ls200_media_frame *frame,
                                                         int *out_has_idr) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  ls200_status status;
  if (out_has_idr == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_has_idr = 0;
  status = ls200_h264_annexb_iterator_init(&iterator, frame->data,
                                           session->config.backend_config->maximum_access_unit_bytes);
  while (status == LS200_STATUS_OK) {
    status = ls200_h264_annexb_iterator_next(&iterator, &nal);
    if (status == LS200_STATUS_OK) {
      if (nal.is_idr != 0) *out_has_idr = 1;
      status = ls200_h264_parameter_sets_update(&session->parameter_sets, &nal);
      if (status != LS200_STATUS_OK) return status;
      if (nal.type == 7U) {
        status = ls200_h264_profile_level_id(&session->parameter_sets,
                                             session->status.active_h264_profile_level_id);
        if (status != LS200_STATUS_OK) return status;
        if (!ls200_media_session_h264_profile_matches(session)) {
          session->video_transmission_ready = 0;
          return LS200_STATUS_INVALID_DATA;
        }
      }
    }
  }
  if (status != LS200_STATUS_END) return status;
  return LS200_STATUS_OK;
}

static ls200_status media_session_require_video_ready(ls200_media_session *session,
                                                      int has_idr) {
  if (session->video_transmission_ready == 0) {
    if (!ls200_h264_can_transmit_access_unit(&session->parameter_sets, has_idr)) {
      (void)ls200_media_session_request_keyframe_internal(session);
      return LS200_STATUS_AGAIN;
    }
    session->video_transmission_ready = 1;
  }
  return LS200_STATUS_OK;
}

static ls200_status media_session_packetize_h264_nal(
    ls200_media_session *session, const ls200_h264_nal *nal,
    const ls200_h264_annexb_iterator *next_iterator,
    ls200_rtp_send_batch *batch) {
  media_session_h264_callback_context callback_context;
  ls200_h264_annexb_iterator lookahead = *next_iterator;
  ls200_h264_nal ignored;
  ls200_status next_status = ls200_h264_annexb_iterator_next(&lookahead, &ignored);
  ls200_status status;
  if (next_status != LS200_STATUS_OK && next_status != LS200_STATUS_END) return next_status;
  callback_context.batch = batch;
  callback_context.final_nal = next_status == LS200_STATUS_END;
  if (nal->length <= (size_t)session->h264_packetizer.mtu - 12U) {
    status = ls200_h264_packetize_single_nal(&session->h264_packetizer, nal,
                                              media_session_h264_packet_callback,
                                              &callback_context);
  } else {
    status = ls200_h264_packetize_fu_a(&session->h264_packetizer, nal,
                                       media_session_h264_packet_callback,
                                       &callback_context);
  }
  return status;
}

static ls200_status media_session_h264_batch_size(
    const ls200_media_session *session, const ls200_media_frame *frame,
    uint32_t *out_packets, uint32_t *out_bytes) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  size_t single_limit = (size_t)session->h264_packetizer.mtu - 12U;
  size_t fragment_limit = (size_t)session->h264_packetizer.mtu - 14U;
  uint64_t packets = 0U;
  uint64_t bytes = 0U;
  ls200_status status = ls200_h264_annexb_iterator_init(
      &iterator, frame->data,
      session->config.backend_config->maximum_access_unit_bytes);
  if (status != LS200_STATUS_OK) return status;
  while ((status = ls200_h264_annexb_iterator_next(&iterator, &nal)) ==
         LS200_STATUS_OK) {
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
      return LS200_STATUS_LIMIT_EXCEEDED;
  }
  if (status != LS200_STATUS_END || packets == 0U)
    return status == LS200_STATUS_END ? LS200_STATUS_INVALID_DATA : status;
  *out_packets = (uint32_t)packets;
  *out_bytes = (uint32_t)bytes;
  return LS200_STATUS_OK;
}

static void media_session_record_access_unit_drop(ls200_media_session *session) {
  if (session->video_access_unit_drops != UINT64_MAX)
    ++session->video_access_unit_drops;
}

static ls200_status media_session_packetize_video_frame(ls200_media_session *session,
                                                         const ls200_media_frame *frame) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  ls200_rtp_send_batch *batch = NULL;
  uint32_t packet_count = 0U;
  uint32_t payload_bytes = 0U;
  uint16_t saved_sequence;
  uint64_t now_ns = 0U;
  ls200_status status = media_session_h264_batch_size(
      session, frame, &packet_count, &payload_bytes);
  if (status != LS200_STATUS_OK) {
    media_session_record_access_unit_drop(session);
    return status;
  }
  (void)ls200_platform_monotonic_now(&now_ns);
  status = ls200_rtp_send_queue_batch_begin(
      session->video.queue, packet_count, payload_bytes, now_ns, &batch);
  if (status != LS200_STATUS_OK) {
    media_session_record_access_unit_drop(session);
    return status;
  }
  status = ls200_h264_annexb_iterator_init(&iterator, frame->data,
      session->config.backend_config->maximum_access_unit_bytes);
  saved_sequence = session->h264_packetizer.sequence_number;
  session->h264_packetizer.timestamp = media_session_video_timestamp(frame->pts_ns);
  while ((status = ls200_h264_annexb_iterator_next(&iterator, &nal)) == LS200_STATUS_OK) {
    status = media_session_packetize_h264_nal(session, &nal, &iterator, batch);
    if (status != LS200_STATUS_OK) break;
  }
  if (status == LS200_STATUS_END) status = ls200_rtp_send_batch_commit(batch);
  if (status == LS200_STATUS_OK) return LS200_STATUS_OK;
  session->h264_packetizer.sequence_number = saved_sequence;
  ls200_rtp_send_batch_abort(batch);
  media_session_record_access_unit_drop(session);
  return status;
}

ls200_status ls200_media_session_pump_video_internal(ls200_media_session *session,
                                                      ls200_deadline deadline) {
  ls200_media_frame frame;
  int has_idr;
  int readiness_before_admission;
  ls200_status status;
  if (!ls200_media_session_direction_sends(session->video_direction) ||
      session->video_transmit_enabled == 0) return LS200_STATUS_AGAIN;
  status = media_session_read_video_frame(session, deadline, &frame);
  if (status == LS200_STATUS_OK) status = media_session_update_h264_parameters(session, &frame, &has_idr);
  if (status == LS200_STATUS_OK) {
    readiness_before_admission = session->video_transmission_ready;
    status = media_session_require_video_ready(session, has_idr);
  }
  if (status == LS200_STATUS_OK) {
    status = media_session_packetize_video_frame(session, &frame);
    if (status != LS200_STATUS_OK)
      session->video_transmission_ready = readiness_before_admission;
    if (status == LS200_STATUS_LIMIT_EXCEEDED) status = LS200_STATUS_OK;
  }
  return status;
}

ls200_status ls200_media_session_flush_stream_internal(
    ls200_media_session *session, ls200_media_session_stream *stream) {
  ls200_rtp_packet packet;
  ls200_rtcp_sender_metrics metrics;
  uint8_t report_data[256];
  ls200_mutable_bytes report = {report_data, sizeof(report_data), 0U};
  uint64_t now_ns = 0U;
  uint64_t ntp_timestamp = 0U;
  ls200_status status;
  (void)ls200_platform_monotonic_now(&now_ns);
  status = ls200_rtp_send_queue_dequeue_bounded(stream->queue, now_ns,
      stream == &session->video ? 4U :
      (stream->queue == stream->dtmf_queue ? 1U : 2U), &packet);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtp_session_send_rtp(stream->transport, &packet);
  if (status != LS200_STATUS_OK) {
    /* Dequeue transfers queue ownership; current transport has no requeue
     * primitive, so a would-block send is an explicit bounded drop. */
    stream->send_drop_packets++;
    return status;
  }
  stream->sent_packets++;
  stream->sent_octets += packet.payload.length;
  status = ls200_platform_ntp_now(&ntp_timestamp);
  if (status != LS200_STATUS_OK) return status;
  (void)memset(&metrics, 0, sizeof(metrics));
  metrics.ssrc = stream->identity.ssrc;
  metrics.ntp_timestamp = ntp_timestamp;
  metrics.rtp_timestamp = packet.header.timestamp;
  metrics.packet_count = stream->sent_packets > UINT32_MAX ? UINT32_MAX :
                         (uint32_t)stream->sent_packets;
  metrics.octet_count = stream->sent_octets > UINT32_MAX ? UINT32_MAX :
                        (uint32_t)stream->sent_octets;
  status = ls200_rtcp_reporter_build_sender_report(stream->reporter, &metrics, &report);
  if (status == LS200_STATUS_OK) status = ls200_rtp_session_send_rtcp(
      stream->transport, (ls200_bytes){report.data, report.length});
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  ls200_media_session_record_activity(session);
  return LS200_STATUS_OK;
}

static int media_session_is_untrusted_datagram_status(ls200_status status) {
  return status == LS200_STATUS_INVALID_DATA || status == LS200_STATUS_LIMIT_EXCEEDED ||
      status == LS200_STATUS_PERMISSION_DENIED || status == LS200_STATUS_SECURITY_ERROR ||
      status == LS200_STATUS_UNSUPPORTED || status == LS200_STATUS_STATE_ERROR;
}

static int media_session_reject_untrusted(ls200_media_session *session, ls200_status status) {
  if (!media_session_is_untrusted_datagram_status(status)) return 0;
  session->rejected_packets++;
  return 1;
}

static ls200_status media_session_queue_received_packet(
    ls200_media_session_stream *stream, const ls200_rtp_packet *packet) {
  uint64_t now_ns = 0U;
  ls200_status status = ls200_platform_monotonic_now(&now_ns);
  if (status != LS200_STATUS_OK) return status;
  return ls200_rx_playout_push(&stream->playout, packet, now_ns);
}

static uint8_t media_session_dtmf_receive_payload(
    const ls200_media_session *session) {
  return session->directional_payloads_configured != 0 ?
      session->dtmf_receive_payload_type : session->dtmf_payload_type;
}

static ls200_status media_session_drain_rtp(ls200_media_session *session,
                                            ls200_media_session_stream *stream,
                                            const ls200_rx_media_profile *receive_profile,
                                            ls200_mutable_bytes *packet_buffer,
                                            int *out_continue) {
  ls200_rtp_source source;
  ls200_rtp_packet packet;
  ls200_status status;
  packet_buffer->length = 0U;
  status = ls200_rtp_session_receive_rtp(stream->transport, packet_buffer, &source, &packet);
  if (status != LS200_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return LS200_STATUS_OK;
    }
    return status;
  }
  status = ls200_rx_shim_accept_rtp_for_media(session->receive_shim, receive_profile, &source,
                                              (ls200_bytes){packet_buffer->data,
                                                            packet_buffer->length});
  if (status != LS200_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return LS200_STATUS_OK;
    }
    return status;
  }
  stream->received_ssrc = source.ssrc;
  if (stream == &session->audio && session->dtmf_enabled != 0 &&
      packet.header.payload_type == media_session_dtmf_receive_payload(session)) {
    ls200_dtmf_event ignored_event;
    status = ls200_dtmf_receiver_accept(session->dtmf_receiver, &packet, &ignored_event);
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) {
      if (media_session_reject_untrusted(session, status)) {
        *out_continue = 1;
        return LS200_STATUS_OK;
      }
      return status;
    }
  } else {
    status = media_session_queue_received_packet(stream, &packet);
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN &&
        status != LS200_STATUS_LIMIT_EXCEEDED) {
      if (media_session_reject_untrusted(session, status)) {
        *out_continue = 1;
        return LS200_STATUS_OK;
      }
      return status;
    }
  }
  session->received_packets++;
  session->drained_bytes += packet_buffer->length;
  return LS200_STATUS_OK;
}

static ls200_status media_session_drain_rtcp(ls200_media_session *session,
                                             ls200_media_session_stream *stream,
                                             ls200_mutable_bytes *packet_buffer,
                                             int receive_rtp,
                                             int *out_continue) {
  ls200_rtp_source source;
  ls200_rtcp_compound_summary summary;
  ls200_status status;
  packet_buffer->length = 0U;
  status = ls200_rtp_session_receive_rtcp(stream->transport, packet_buffer, &source);
  if (status != LS200_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return LS200_STATUS_OK;
    }
    return status;
  }
  if (ls200_rtcp_visit_feedback(
          (ls200_bytes){packet_buffer->data, packet_buffer->length}, NULL, NULL) !=
      LS200_STATUS_OK) {
    session->rejected_packets++;
    *out_continue = 1;
    return LS200_STATUS_OK;
  }
  source.ssrc = ((uint32_t)packet_buffer->data[4] << 24) |
                ((uint32_t)packet_buffer->data[5] << 16) |
                ((uint32_t)packet_buffer->data[6] << 8) | packet_buffer->data[7];
  if (receive_rtp != 0) {
    status = ls200_rx_shim_accept_rtcp_source_internal(
        session->receive_shim, stream->receive_media_id, &source,
        (ls200_bytes){packet_buffer->data, packet_buffer->length}, &summary);
  } else {
    status = ls200_rtcp_parse_compound(
        (ls200_bytes){packet_buffer->data, packet_buffer->length}, &summary);
  }
  if (status == LS200_STATUS_OK)
    status = ls200_media_session_accept_feedback_internal(session, stream, source.ssrc,
        (ls200_bytes){packet_buffer->data, packet_buffer->length});
  if (status == LS200_STATUS_AGAIN) {
    *out_continue = 1;
    return LS200_STATUS_OK;
  }
  if (status != LS200_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return LS200_STATUS_OK;
    }
    return status;
  }
  if (receive_rtp == 0) ls200_media_session_report_rtcp_internal(
      session, stream, (ls200_bytes){packet_buffer->data, packet_buffer->length});
  session->received_packets++;
  session->drained_bytes += packet_buffer->length;
  return LS200_STATUS_OK;
}

static int media_session_stream_receives_rtp(
    const ls200_media_session *session, const ls200_media_session_stream *stream) {
  ls200_sdp_direction direction = stream == &session->video ?
      session->video_direction : session->audio_direction;
  return direction == LS200_SDP_SENDRECV || direction == LS200_SDP_RECVONLY;
}

static ls200_status media_session_discard_rtp(
    ls200_media_session *session, ls200_media_session_stream *stream,
    ls200_mutable_bytes *packet_buffer, int *out_continue) {
  ls200_rtp_source source;
  ls200_rtp_packet packet;
  ls200_status status;
  packet_buffer->length = 0U;
  status = ls200_rtp_session_receive_rtp(stream->transport, packet_buffer,
                                         &source, &packet);
  if (status != LS200_STATUS_OK) {
    if (media_session_reject_untrusted(session, status)) {
      *out_continue = 1;
      return LS200_STATUS_OK;
    }
    return status;
  }
  session->rejected_packets++;
  session->drained_bytes += packet_buffer->length;
  *out_continue = 1;
  return LS200_STATUS_OK;
}

ls200_status ls200_media_session_drain_stream_internal(
    ls200_media_session *session, ls200_media_session_stream *stream) {
  uint8_t packet_storage[LS200_SIPD_MAX_RTP_PACKET_BYTES];
  ls200_mutable_bytes packet_buffer = {packet_storage, session->config.rtp_mtu, 0U};
  ls200_rx_media_profile receive_profile;
  int receive_rtp;
  unsigned int count;
  receive_profile.media_id = stream->receive_media_id;
  receive_profile.payload_clocks = stream->payload_clocks;
  receive_profile.payload_clock_count = stream->payload_type_count;
  receive_rtp = media_session_stream_receives_rtp(session, stream);
  for (count = 0U; count < LS200_MEDIA_SESSION_MAX_INGRESS_PER_POLL; ++count) {
    int rtp_ready = 0;
    int rtcp_ready = 0;
    int continue_drain = 0;
    ls200_status status = ls200_rtp_session_poll(stream->transport, &rtp_ready, &rtcp_ready);
    if (status == LS200_STATUS_AGAIN) return count == 0U ? LS200_STATUS_AGAIN : LS200_STATUS_OK;
    if (status != LS200_STATUS_OK) return status;
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
    if (status != LS200_STATUS_OK) return status;
    if (continue_drain != 0) continue;
    ls200_media_session_record_activity(session);
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_media_session_emit_receive_report_internal(
    ls200_media_session *session, ls200_media_session_stream *stream) {
  uint8_t report_data[256];
  ls200_mutable_bytes report = {report_data, sizeof(report_data), 0U};
  ls200_rtcp_report_metrics metrics;
  ls200_status status;
  if (session == NULL || stream == NULL || session->receive_shim == NULL ||
      stream->transport == NULL || stream->reporter == NULL || stream->received_ssrc == 0U) {
    return LS200_STATUS_AGAIN;
  }
  status = ls200_rx_shim_get_receiver_report_metrics_for_media(
      session->receive_shim, stream->receive_media_id, stream->received_ssrc,
      stream->identity.ssrc, &metrics);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtcp_reporter_build_receiver_report(stream->reporter, &metrics, &report);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtp_session_send_rtcp(stream->transport,
                                       (ls200_bytes){report.data, report.length});
  if (status != LS200_STATUS_OK) return status;
  return ls200_rx_shim_commit_receiver_report_for_media(session->receive_shim,
                                                         stream->receive_media_id,
                                                         stream->received_ssrc);
}

ls200_status ls200_media_session_send_receive_bye(ls200_media_session *session) {
  uint8_t output_data[16];
  ls200_mutable_bytes output = {output_data, sizeof(output_data), 0U};
  ls200_status status;
  if (session == NULL || session->receive_shim == NULL || session->video.transport == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_rx_shim_stop(session->receive_shim, &output);
  if (status != LS200_STATUS_OK) return status;
  return ls200_rtp_session_send_rtcp(session->video.transport,
                                     (ls200_bytes){output.data, output.length});
}
