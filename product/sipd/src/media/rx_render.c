#include "session_private.h"
#include "ls200_sipd/platform.h"
#include "ls200_sipd/rtcp.h"
#include <string.h>

static void request_pli(ls200_media_session *session, uint64_t now_ns);

static ls200_status recover_video_packet(ls200_media_session *session,
                                          ls200_status status) {
  uint64_t now_ns = 0U;
  if (status != LS200_STATUS_INVALID_DATA && status != LS200_STATUS_UNSUPPORTED &&
      status != LS200_STATUS_LIMIT_EXCEEDED) return status;
  session->receive_video_waiting_for_idr = 1;
  if (ls200_platform_monotonic_now(&now_ns) == LS200_STATUS_OK)
    request_pli(session, now_ns);
  return LS200_STATUS_AGAIN;
}

static int receives(ls200_sdp_direction direction) {
  return direction == LS200_SDP_SENDRECV || direction == LS200_SDP_RECVONLY;
}

static void reset_aec_reference(ls200_media_session *session) {
  ls200_aec_reset(session->aec);
  session->aec_reference_remainder_length = 0U;
  (void)memset(session->aec_reference_remainder, 0,
               sizeof(session->aec_reference_remainder));
}

static void buffer_aec_reference(ls200_media_session *session, ls200_bytes decoded) {
  size_t offset = 0U;
  while (session->aec != NULL && offset < decoded.length) {
    size_t available = decoded.length - offset;
    size_t needed = LS200_AEC_FRAME_BYTES - session->aec_reference_remainder_length;
    size_t copied = available < needed ? available : needed;
    (void)memcpy(session->aec_reference_remainder + session->aec_reference_remainder_length,
                 decoded.data + offset, copied);
    session->aec_reference_remainder_length += copied;
    offset += copied;
    if (session->aec_reference_remainder_length == LS200_AEC_FRAME_BYTES) {
      (void)ls200_aec_push_reference(
          session->aec, (ls200_bytes){session->aec_reference_remainder,
                                      session->aec_reference_remainder_length});
      session->aec_reference_remainder_length = 0U;
    }
  }
}

static ls200_status cache_parameter_sets(ls200_media_session *session, ls200_bytes access_unit,
                                         int *out_idr, int *out_sps, int *out_pps) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  ls200_status status;
  *out_idr = 0;
  *out_sps = 0;
  *out_pps = 0;
  status = ls200_h264_annexb_iterator_init(&iterator, access_unit,
                                            (uint32_t)session->receive_video_capacity);
  while (status == LS200_STATUS_OK) {
    status = ls200_h264_annexb_iterator_next(&iterator, &nal);
    if (status != LS200_STATUS_OK) break;
    if (nal.is_idr != 0) *out_idr = 1;
    if (nal.type == 7U) *out_sps = 1;
    if (nal.type == 8U) *out_pps = 1;
    status = ls200_h264_parameter_sets_update(&session->receive_parameter_sets, &nal);
  }
  return status == LS200_STATUS_END ? LS200_STATUS_OK : status;
}

static ls200_status restore_parameter_sets(ls200_media_session *session,
                                           ls200_bytes access_unit, int has_sps, int has_pps,
                                           ls200_bytes *out_access_unit) {
  static const uint8_t start_code[4] = {0U, 0U, 0U, 1U};
  size_t offset = 0U;
  if (has_sps != 0 && has_pps != 0) {
    *out_access_unit = access_unit;
    return LS200_STATUS_OK;
  }
  if ((has_sps == 0 && session->receive_parameter_sets.sps_length == 0U) ||
      (has_pps == 0 && session->receive_parameter_sets.pps_length == 0U)) return LS200_STATUS_AGAIN;
  if (access_unit.length > session->receive_video_render_capacity - 8U -
      session->receive_parameter_sets.sps_length - session->receive_parameter_sets.pps_length) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  if (has_sps == 0) {
    (void)memcpy(session->receive_video_render + offset, start_code, sizeof(start_code));
    offset += sizeof(start_code);
    (void)memcpy(session->receive_video_render + offset, session->receive_parameter_sets.sps,
                 session->receive_parameter_sets.sps_length);
    offset += session->receive_parameter_sets.sps_length;
  }
  if (has_pps == 0) {
    (void)memcpy(session->receive_video_render + offset, start_code, sizeof(start_code));
    offset += sizeof(start_code);
    (void)memcpy(session->receive_video_render + offset, session->receive_parameter_sets.pps,
                 session->receive_parameter_sets.pps_length);
    offset += session->receive_parameter_sets.pps_length;
  }
  (void)memcpy(session->receive_video_render + offset, access_unit.data, access_unit.length);
  *out_access_unit = (ls200_bytes){session->receive_video_render, offset + access_unit.length};
  return LS200_STATUS_OK;
}

static ls200_status render_audio(ls200_media_session *session, const ls200_rtp_packet *packet) {
  uint8_t decoded_pcm[LS200_SIPD_MAX_RTP_PACKET_BYTES * 2U];
  ls200_mutable_bytes decoded = {decoded_pcm, sizeof(decoded_pcm), 0U};
  ls200_media_frame frame;
  size_t input_sample;
  size_t output_offset = 0U;
  uint8_t receive_payload_type = session->directional_payloads_configured != 0 ?
      session->audio_receive_payload_type : session->audio_payload_type;
  ls200_status status = packet->header.payload_type != receive_payload_type ?
      LS200_STATUS_OK : (session->use_pcma != 0 ? ls200_g711_decode_pcma(packet->payload, &decoded) :
      ls200_g711_decode_pcmu(packet->payload, &decoded));
  if (status != LS200_STATUS_OK) return status;
  if (decoded.length % 2U != 0U || decoded.length > session->receive_audio_capacity / 12U)
    return LS200_STATUS_LIMIT_EXCEEDED;
  for (input_sample = 0U; input_sample < decoded.length; input_sample += 2U) {
    unsigned int repeat;
    for (repeat = 0U; repeat < 6U; ++repeat) {
      (void)memcpy(session->receive_audio + output_offset, decoded.data + input_sample, 2U);
      (void)memcpy(session->receive_audio + output_offset + 2U, decoded.data + input_sample, 2U);
      output_offset += 4U;
    }
  }
  (void)memset(&frame, 0, sizeof(frame));
  frame.kind = LS200_MEDIA_AUDIO_PCM_S16LE;
  frame.data = (ls200_bytes){session->receive_audio, output_offset};
  frame.pts_ns = (uint64_t)packet->header.timestamp * UINT64_C(1000000000) / LS200_G711_SAMPLE_RATE;
  frame.sample_rate = 48000U;
  frame.channels = 2U;
  status = session->config.renderer->vtable->render(session->config.renderer, &frame);
  if (status == LS200_STATUS_OK) {
    buffer_aec_reference(session, (ls200_bytes){decoded.data, decoded.length});
  }
  return status;
}

static ls200_status render_video(ls200_media_session *session, const ls200_rtp_packet *packet) {
  ls200_mutable_bytes access_unit = {session->receive_video, session->receive_video_capacity, 0U};
  ls200_media_frame frame;
  ls200_bytes render_access_unit;
  int complete = 0;
  int has_idr;
  int has_sps;
  int has_pps;
  ls200_status status;
  uint8_t receive_payload_type = session->directional_payloads_configured != 0 ?
      session->video_receive_payload_type : session->video_payload_type;
  if (packet->header.payload_type != receive_payload_type)
    return LS200_STATUS_OK;
  status = ls200_h264_depacketizer_push(session->h264_depacketizer, packet, &access_unit, &complete);
  if (status != LS200_STATUS_OK) return recover_video_packet(session, status);
  if (access_unit.length == 0U || complete == 0) return LS200_STATUS_AGAIN;
  status = cache_parameter_sets(session, (ls200_bytes){access_unit.data, access_unit.length},
                                &has_idr, &has_sps, &has_pps);
  if (status != LS200_STATUS_OK) return status;
  if (has_idr != 0) {
    status = restore_parameter_sets(session, (ls200_bytes){access_unit.data, access_unit.length},
                                    has_sps, has_pps, &render_access_unit);
    if (status != LS200_STATUS_OK) return status;
  } else render_access_unit = (ls200_bytes){access_unit.data, access_unit.length};
  (void)memset(&frame, 0, sizeof(frame));
  frame.kind = LS200_MEDIA_VIDEO_H264_ANNEX_B;
  frame.data = render_access_unit;
  frame.pts_ns = (uint64_t)packet->header.timestamp * UINT64_C(1000000000) / UINT64_C(90000);
  frame.keyframe = has_idr;
  if (frame.keyframe == 0 && session->receive_video_waiting_for_idr != 0) return LS200_STATUS_AGAIN;
  if (frame.keyframe != 0) session->receive_video_waiting_for_idr = 0;
  status = session->config.renderer->vtable->render(session->config.renderer, &frame);
  return status == LS200_STATUS_LIMIT_EXCEEDED ? recover_video_packet(session, status) : status;
}

static ls200_status render_packet(ls200_media_session *session, ls200_media_session_stream *stream,
                                  const ls200_rtp_packet *packet) {
  ls200_status status;
  if (session->config.renderer == NULL) return LS200_STATUS_OK;
  if (stream == &session->audio && receives(session->audio_direction)) status = render_audio(session, packet);
  else if (stream == &session->video && receives(session->video_direction)) status = render_video(session, packet);
  else return LS200_STATUS_OK;
  return status;
}

static void account_render(ls200_media_session *session, ls200_media_session_stream *stream,
                           ls200_status status, uint64_t now_ns) {
  int audio = stream == &session->audio;
  if (status == LS200_STATUS_OK) {
    session->status.rx_renderer_healthy = 1;
    if (audio) {
      session->status.rx_audio_frames_rendered++;
      session->status.rx_audio_rendering = 1;
      session->status.rx_audio_render_fresh = 1;
      session->status.rx_audio_last_success_ns = now_ns;
    } else {
      session->status.rx_video_frames_rendered++;
      session->status.rx_video_rendering = 1;
      session->status.rx_video_render_fresh = 1;
      session->status.rx_video_last_success_ns = now_ns;
    }
  } else if (status == LS200_STATUS_AGAIN || status == LS200_STATUS_LIMIT_EXCEEDED) {
    if (audio) session->status.rx_audio_frames_dropped++; else session->status.rx_video_frames_dropped++;
    if (audio) {
      session->status.rx_audio_rendering = 0;
      session->status.rx_audio_render_fresh = 0;
    } else {
      session->status.rx_video_rendering = 0;
      session->status.rx_video_render_fresh = 0;
    }
  } else {
    session->status.rx_renderer_failures++;
    session->status.rx_renderer_healthy = 0;
    if (audio) {
      session->status.rx_audio_rendering = 0;
      session->status.rx_audio_render_fresh = 0;
    } else {
      session->status.rx_video_rendering = 0;
      session->status.rx_video_render_fresh = 0;
    }
  }
  session->status.rx_rendering = session->status.rx_audio_rendering != 0 ||
      session->status.rx_video_rendering != 0;
}

static void account_underrun(ls200_media_session *session,
                             ls200_media_session_stream *stream) {
  if (stream == &session->audio) {
    session->status.rx_audio_rendering = 0;
    session->status.rx_audio_render_fresh = 0;
    reset_aec_reference(session);
  } else {
    session->status.rx_video_rendering = 0;
    session->status.rx_video_render_fresh = 0;
  }
  session->status.rx_rendering = session->status.rx_audio_rendering != 0 ||
      session->status.rx_video_rendering != 0;
}

static void request_pli(ls200_media_session *session, uint64_t now_ns) {
  uint8_t wire[16];
  ls200_mutable_bytes output = {wire, sizeof(wire), 0U};
  ls200_rtcp_feedback feedback = {session->video.identity.ssrc, session->video.received_ssrc, 1, 0};
  ls200_rtcp_feedback_policy policy = {0, 0, 1, 0};
  if (session->video.transport == NULL || feedback.media_ssrc == 0U ||
      (session->last_receive_pli_ns != 0U && now_ns >= session->last_receive_pli_ns &&
       now_ns - session->last_receive_pli_ns < (uint64_t)LS200_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS * UINT64_C(1000000))) return;
  if (ls200_rtcp_build_feedback(&feedback, &policy, &output) == LS200_STATUS_OK &&
      ls200_rtp_session_send_rtcp(session->video.transport, (ls200_bytes){output.data, output.length}) == LS200_STATUS_OK)
    session->last_receive_pli_ns = now_ns;
}

ls200_status ls200_media_session_playout_stream_internal(ls200_media_session *session,
                                                          ls200_media_session_stream *stream) {
  uint8_t payload[LS200_SIPD_MAX_RTP_PACKET_BYTES - 12U];
  ls200_rtp_packet packet;
  uint64_t now_ns = 0U;
  uint64_t previous_underruns;
  int discontinuity = 0;
  ls200_status status;
  if (session == NULL || stream == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK) return LS200_STATUS_IO_ERROR;
  previous_underruns = stream->playout.stats.underruns;
  status = ls200_rx_playout_take(&stream->playout, now_ns, &packet, payload, sizeof(payload), &discontinuity);
  if (status != LS200_STATUS_OK) {
    if (stream == &session->audio) session->status.rx_audio_underruns = stream->playout.stats.underruns;
    else session->status.rx_video_underruns = stream->playout.stats.underruns;
    if (stream->playout.stats.underruns != previous_underruns) account_underrun(session, stream);
  } else {
    if (discontinuity != 0 && stream == &session->video) { session->receive_video_waiting_for_idr = 1; request_pli(session, now_ns); }
    if (discontinuity != 0 && stream == &session->audio) reset_aec_reference(session);
    status = render_packet(session, stream, &packet);
    if (stream == &session->audio && status != LS200_STATUS_OK) reset_aec_reference(session);
    account_render(session, stream, status, now_ns);
    if (status == LS200_STATUS_AGAIN) status = LS200_STATUS_OK;
  }
  session->status.rx_reorder_drops = session->audio.playout.stats.dropped_packets + session->video.playout.stats.dropped_packets;
  return status;
}
