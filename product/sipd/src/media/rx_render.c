#include "session_private.h"
#include "aula_sipd/platform.h"
#include "aula_sipd/rtcp.h"
#include <string.h>

static void request_pli(aula_media_session *session, uint64_t now_ns);

static aula_status recover_video_packet(aula_media_session *session,
                                          aula_status status) {
  uint64_t now_ns = 0U;
  if (status != AULA_STATUS_INVALID_DATA && status != AULA_STATUS_UNSUPPORTED &&
      status != AULA_STATUS_LIMIT_EXCEEDED) return status;
  session->receive_video_waiting_for_idr = 1;
  if (aula_platform_monotonic_now(&now_ns) == AULA_STATUS_OK)
    request_pli(session, now_ns);
  return AULA_STATUS_AGAIN;
}

static int receives(aula_sdp_direction direction) {
  return direction == AULA_SDP_SENDRECV || direction == AULA_SDP_RECVONLY;
}

static void reset_aec_reference(aula_media_session *session) {
  aula_aec_reset(session->aec);
  session->aec_reference_remainder_length = 0U;
  (void)memset(session->aec_reference_remainder, 0,
               sizeof(session->aec_reference_remainder));
}

static void buffer_aec_reference(aula_media_session *session, aula_bytes decoded) {
  size_t offset = 0U;
  while (session->aec != NULL && offset < decoded.length) {
    size_t available = decoded.length - offset;
    size_t needed = AULA_AEC_FRAME_BYTES - session->aec_reference_remainder_length;
    size_t copied = available < needed ? available : needed;
    (void)memcpy(session->aec_reference_remainder + session->aec_reference_remainder_length,
                 decoded.data + offset, copied);
    session->aec_reference_remainder_length += copied;
    offset += copied;
    if (session->aec_reference_remainder_length == AULA_AEC_FRAME_BYTES) {
      (void)aula_aec_push_reference(
          session->aec, (aula_bytes){session->aec_reference_remainder,
                                      session->aec_reference_remainder_length});
      session->aec_reference_remainder_length = 0U;
    }
  }
}

static aula_status cache_parameter_sets(aula_media_session *session, aula_bytes access_unit,
                                         int *out_idr, int *out_sps, int *out_pps) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  aula_status status;
  *out_idr = 0;
  *out_sps = 0;
  *out_pps = 0;
  status = aula_h264_annexb_iterator_init(&iterator, access_unit,
                                            (uint32_t)session->receive_video_capacity);
  while (status == AULA_STATUS_OK) {
    status = aula_h264_annexb_iterator_next(&iterator, &nal);
    if (status != AULA_STATUS_OK) break;
    if (nal.is_idr != 0) *out_idr = 1;
    if (nal.type == 7U) *out_sps = 1;
    if (nal.type == 8U) *out_pps = 1;
    status = aula_h264_parameter_sets_update(&session->receive_parameter_sets, &nal);
  }
  return status == AULA_STATUS_END ? AULA_STATUS_OK : status;
}

static aula_status restore_parameter_sets(aula_media_session *session,
                                           aula_bytes access_unit, int has_sps, int has_pps,
                                           aula_bytes *out_access_unit) {
  static const uint8_t start_code[4] = {0U, 0U, 0U, 1U};
  size_t offset = 0U;
  if (has_sps != 0 && has_pps != 0) {
    *out_access_unit = access_unit;
    return AULA_STATUS_OK;
  }
  if ((has_sps == 0 && session->receive_parameter_sets.sps_length == 0U) ||
      (has_pps == 0 && session->receive_parameter_sets.pps_length == 0U)) return AULA_STATUS_AGAIN;
  if (access_unit.length > session->receive_video_render_capacity - 8U -
      session->receive_parameter_sets.sps_length - session->receive_parameter_sets.pps_length) {
    return AULA_STATUS_LIMIT_EXCEEDED;
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
  *out_access_unit = (aula_bytes){session->receive_video_render, offset + access_unit.length};
  return AULA_STATUS_OK;
}

static aula_status render_audio(aula_media_session *session, const aula_rtp_packet *packet) {
  uint8_t decoded_pcm[AULA_SIPD_MAX_RTP_PACKET_BYTES * 2U];
  aula_mutable_bytes decoded = {decoded_pcm, sizeof(decoded_pcm), 0U};
  aula_media_frame frame;
  size_t input_sample;
  size_t output_offset = 0U;
  uint8_t receive_payload_type = session->directional_payloads_configured != 0 ?
      session->audio_receive_payload_type : session->audio_payload_type;
  aula_status status = packet->header.payload_type != receive_payload_type ?
      AULA_STATUS_OK : (session->use_pcma != 0 ? aula_g711_decode_pcma(packet->payload, &decoded) :
      aula_g711_decode_pcmu(packet->payload, &decoded));
  if (status != AULA_STATUS_OK) return status;
  if (decoded.length % 2U != 0U || decoded.length > session->receive_audio_capacity / 12U)
    return AULA_STATUS_LIMIT_EXCEEDED;
  for (input_sample = 0U; input_sample < decoded.length; input_sample += 2U) {
    unsigned int repeat;
    for (repeat = 0U; repeat < 6U; ++repeat) {
      (void)memcpy(session->receive_audio + output_offset, decoded.data + input_sample, 2U);
      (void)memcpy(session->receive_audio + output_offset + 2U, decoded.data + input_sample, 2U);
      output_offset += 4U;
    }
  }
  (void)memset(&frame, 0, sizeof(frame));
  frame.kind = AULA_MEDIA_AUDIO_PCM_S16LE;
  frame.data = (aula_bytes){session->receive_audio, output_offset};
  frame.pts_ns = (uint64_t)packet->header.timestamp * UINT64_C(1000000000) / AULA_G711_SAMPLE_RATE;
  frame.sample_rate = 48000U;
  frame.channels = 2U;
  status = session->config.renderer->vtable->render(session->config.renderer, &frame);
  if (status == AULA_STATUS_OK) {
    buffer_aec_reference(session, (aula_bytes){decoded.data, decoded.length});
  }
  return status;
}

static aula_status render_video(aula_media_session *session, const aula_rtp_packet *packet) {
  aula_mutable_bytes access_unit = {session->receive_video, session->receive_video_capacity, 0U};
  aula_media_frame frame;
  aula_bytes render_access_unit;
  int complete = 0;
  int has_idr;
  int has_sps;
  int has_pps;
  aula_status status;
  uint8_t receive_payload_type = session->directional_payloads_configured != 0 ?
      session->video_receive_payload_type : session->video_payload_type;
  if (packet->header.payload_type != receive_payload_type)
    return AULA_STATUS_OK;
  status = aula_h264_depacketizer_push(session->h264_depacketizer, packet, &access_unit, &complete);
  if (status != AULA_STATUS_OK) return recover_video_packet(session, status);
  if (access_unit.length == 0U || complete == 0) return AULA_STATUS_AGAIN;
  status = cache_parameter_sets(session, (aula_bytes){access_unit.data, access_unit.length},
                                &has_idr, &has_sps, &has_pps);
  if (status != AULA_STATUS_OK) return status;
  if (has_idr != 0) {
    status = restore_parameter_sets(session, (aula_bytes){access_unit.data, access_unit.length},
                                    has_sps, has_pps, &render_access_unit);
    if (status != AULA_STATUS_OK) return status;
  } else render_access_unit = (aula_bytes){access_unit.data, access_unit.length};
  (void)memset(&frame, 0, sizeof(frame));
  frame.kind = AULA_MEDIA_VIDEO_H264_ANNEX_B;
  frame.data = render_access_unit;
  frame.pts_ns = (uint64_t)packet->header.timestamp * UINT64_C(1000000000) / UINT64_C(90000);
  frame.keyframe = has_idr;
  if (frame.keyframe == 0 && session->receive_video_waiting_for_idr != 0) return AULA_STATUS_AGAIN;
  if (frame.keyframe != 0) session->receive_video_waiting_for_idr = 0;
  status = session->config.renderer->vtable->render(session->config.renderer, &frame);
  return status == AULA_STATUS_LIMIT_EXCEEDED ? recover_video_packet(session, status) : status;
}

static aula_status render_packet(aula_media_session *session, aula_media_session_stream *stream,
                                  const aula_rtp_packet *packet) {
  aula_status status;
  if (session->config.renderer == NULL) return AULA_STATUS_OK;
  if (stream == &session->audio && receives(session->audio_direction)) status = render_audio(session, packet);
  else if (stream == &session->video && receives(session->video_direction)) status = render_video(session, packet);
  else return AULA_STATUS_OK;
  return status;
}

static void account_render(aula_media_session *session, aula_media_session_stream *stream,
                           aula_status status, uint64_t now_ns) {
  int audio = stream == &session->audio;
  if (status == AULA_STATUS_OK) {
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
  } else if (status == AULA_STATUS_AGAIN || status == AULA_STATUS_LIMIT_EXCEEDED) {
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

static void account_underrun(aula_media_session *session,
                             aula_media_session_stream *stream) {
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

static void request_pli(aula_media_session *session, uint64_t now_ns) {
  uint8_t wire[16];
  aula_mutable_bytes output = {wire, sizeof(wire), 0U};
  aula_rtcp_feedback feedback = {session->video.identity.ssrc, session->video.received_ssrc, 1, 0};
  aula_rtcp_feedback_policy policy = {0, 0, 1, 0};
  if (session->video.transport == NULL || feedback.media_ssrc == 0U ||
      (session->last_receive_pli_ns != 0U && now_ns >= session->last_receive_pli_ns &&
       now_ns - session->last_receive_pli_ns < (uint64_t)AULA_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS * UINT64_C(1000000))) return;
  if (aula_rtcp_build_feedback(&feedback, &policy, &output) == AULA_STATUS_OK &&
      aula_rtp_session_send_rtcp(session->video.transport, (aula_bytes){output.data, output.length}) == AULA_STATUS_OK)
    session->last_receive_pli_ns = now_ns;
}

aula_status aula_media_session_playout_stream_internal(aula_media_session *session,
                                                          aula_media_session_stream *stream) {
  uint8_t payload[AULA_SIPD_MAX_RTP_PACKET_BYTES - 12U];
  aula_rtp_packet packet;
  uint64_t now_ns = 0U;
  uint64_t previous_underruns;
  int discontinuity = 0;
  aula_status status;
  if (session == NULL || stream == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK) return AULA_STATUS_IO_ERROR;
  previous_underruns = stream->playout.stats.underruns;
  status = aula_rx_playout_take(&stream->playout, now_ns, &packet, payload, sizeof(payload), &discontinuity);
  if (status != AULA_STATUS_OK) {
    if (stream == &session->audio) session->status.rx_audio_underruns = stream->playout.stats.underruns;
    else session->status.rx_video_underruns = stream->playout.stats.underruns;
    if (stream->playout.stats.underruns != previous_underruns) account_underrun(session, stream);
  } else {
    if (discontinuity != 0 && stream == &session->video) { session->receive_video_waiting_for_idr = 1; request_pli(session, now_ns); }
    if (discontinuity != 0 && stream == &session->audio) reset_aec_reference(session);
    status = render_packet(session, stream, &packet);
    if (stream == &session->audio && status != AULA_STATUS_OK) reset_aec_reference(session);
    account_render(session, stream, status, now_ns);
    if (status == AULA_STATUS_AGAIN) status = AULA_STATUS_OK;
  }
  session->status.rx_reorder_drops = session->audio.playout.stats.dropped_packets + session->video.playout.stats.dropped_packets;
  return status;
}
