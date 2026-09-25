#include "session_private.h"
#include "h264_profile_private.h"
#include "ls200_sipd/platform.h"
#include "../rtp/srtp_private.h"
#include "../rtp/payload_private.h"
#include "../sdp/negotiated_private.h"
#include "../sdp/srtp_limits.h"
#include <arpa/inet.h>
#include <string.h>

static void media_session_secure_zero(void *memory, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

static ls200_status media_session_select_reserved_video(
    const ls200_sdp_media_capabilities *capabilities, uint8_t *out_payload_type) {
  size_t index;
  if (capabilities == NULL || capabilities->codecs == NULL || out_payload_type == NULL ||
      !ls200_media_session_direction_sends(capabilities->direction)) return LS200_STATUS_INVALID_ARGUMENT;
  for (index = 0U; index < capabilities->codec_count; ++index) {
    if (capabilities->codecs[index].kind == LS200_SDP_CODEC_H264 &&
        capabilities->codecs[index].clock_rate == 90000U) {
      *out_payload_type = capabilities->codecs[index].payload_type;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_UNSUPPORTED;
}

static ls200_status media_session_select_reserved_audio(
    const ls200_sdp_media_capabilities *capabilities, uint8_t *out_payload_type,
    int *out_use_pcma) {
  size_t index;
  if (capabilities == NULL || capabilities->codecs == NULL || out_payload_type == NULL ||
      out_use_pcma == NULL || !ls200_media_session_direction_sends(capabilities->direction)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0U; index < capabilities->codec_count; ++index) {
    const ls200_sdp_codec *codec = &capabilities->codecs[index];
    if (codec->clock_rate != LS200_G711_SAMPLE_RATE || codec->channels != 1U) continue;
    if (codec->kind == LS200_SDP_CODEC_PCMU || codec->kind == LS200_SDP_CODEC_PCMA) {
      *out_payload_type = codec->payload_type;
      *out_use_pcma = codec->kind == LS200_SDP_CODEC_PCMA;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_UNSUPPORTED;
}

static ls200_status media_session_parse_address(const char *address, uint8_t *out_address,
                                                uint8_t *out_address_length) {
  int result;
  if (address == NULL || address[0] == '\0' || out_address == NULL ||
      out_address_length == NULL) return LS200_STATUS_INVALID_DATA;
  result = inet_pton(AF_INET, address, out_address);
  if (result == 1) {
    *out_address_length = 4U;
    return LS200_STATUS_OK;
  }
  result = inet_pton(AF_INET6, address, out_address);
  if (result != 1) return LS200_STATUS_INVALID_DATA;
  *out_address_length = 16U;
  return LS200_STATUS_OK;
}

static ls200_status media_session_remote_target(const ls200_sdp_negotiated_media *media,
                                                ls200_rtp_remote_target *out_target) {
  ls200_status status;
  if (media == NULL || out_target == NULL || media->remote_rtp_port == 0U ||
      media->remote_rtp_address[0] == '\0' ||
      (!media->remote_rtcp_mux && (media->remote_rtcp_port == 0U ||
                                   media->remote_rtcp_address[0] == '\0'))) {
    return LS200_STATUS_INVALID_DATA;
  }
  (void)memset(out_target, 0, sizeof(*out_target));
  status = media_session_parse_address(media->remote_rtp_address, out_target->rtp_address,
                                       &out_target->rtp_address_length);
  if (status != LS200_STATUS_OK) return status;
  out_target->rtp_port = media->remote_rtp_port;
  out_target->rtcp_mux = media->remote_rtcp_mux != 0;
  if (out_target->rtcp_mux != 0) {
    (void)memcpy(out_target->rtcp_address, out_target->rtp_address,
                 out_target->rtp_address_length);
    out_target->rtcp_address_length = out_target->rtp_address_length;
    out_target->rtcp_port = out_target->rtp_port;
  } else {
    status = media_session_parse_address(media->remote_rtcp_address, out_target->rtcp_address,
                                         &out_target->rtcp_address_length);
    if (status != LS200_STATUS_OK) return status;
    out_target->rtcp_port = media->remote_rtcp_port;
  }
  return LS200_STATUS_OK;
}

static int media_session_payload_was_reserved(const ls200_media_session_stream *stream,
                                              uint8_t payload_type) {
  size_t index;
  for (index = 0U; stream != NULL && index < stream->payload_type_count; ++index) {
    if (stream->payload_types[index] == payload_type) return 1;
  }
  return 0;
}

static ls200_status media_session_create_stream(ls200_media_session *session,
                                                ls200_media_session_stream *stream,
                                                const ls200_sdp_media_capabilities *capabilities,
                                                uint8_t payload_type, uint64_t pacing_ns,
                                                uint32_t maximum_queue_packets) {
  uint8_t random_bytes[10];
  ls200_mutable_bytes random_output = {random_bytes, sizeof(random_bytes), 0U};
  ls200_rtp_transport_config transport;
  ls200_rtp_session_policy policy;
  ls200_status status;
  if (session == NULL || stream == NULL || capabilities == NULL || capabilities->codecs == NULL ||
      capabilities->codec_count == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_media_session_collect_payloads(
      capabilities->codecs, capabilities->codec_count, stream);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_platform_random_bytes(&random_output);
  if (status == LS200_STATUS_OK) {
    status = ls200_rtp_identity_create((ls200_bytes){random_bytes, sizeof(random_bytes)},
                                       payload_type, &stream->identity);
  }
  (void)memset(random_bytes, 0, sizeof(random_bytes));
  if (status != LS200_STATUS_OK) return status;
  transport = session->config.transport;
  transport.rtcp_mux = capabilities->ports.rtcp_mux != 0;
  (void)memset(&policy, 0, sizeof(policy));
  policy.accepted_payloads.payload_types = stream->payload_types;
  policy.accepted_payloads.payload_type_count = stream->payload_type_count; policy.peer.bind_address = session->config.transport.local_address;
  policy.peer.bind_address_length = session->config.transport.local_address_length;
  policy.peer.expected_peer = NULL; policy.peer.expected_peer_authenticated = 0;
  policy.peer.symmetric_rtp_enabled = session->config.symmetric_rtp_enabled != 0;
  policy.maximum_packet_bytes = session->config.rtp_mtu;
  status = ls200_rtp_session_reserve(&transport, &stream->identity, &policy, &stream->transport);
  if (status != LS200_STATUS_OK) return status;
  (void)pacing_ns;
  (void)maximum_queue_packets;
  return LS200_STATUS_OK;
}

static ls200_status media_session_prepare_stream_runtime(
    ls200_media_session *session, ls200_media_session_stream *stream,
    uint64_t pacing_ns, uint32_t maximum_queue_packets) {
  ls200_rtp_pacing_config pacing;
  ls200_rtcp_reporter_config reporter_config;
  ls200_status status;
  pacing.packet_interval_ns = pacing_ns;
  pacing.maximum_queue_packets = maximum_queue_packets;
  pacing.maximum_queue_bytes = session->config.maximum_queue_bytes;
  status = ls200_rtp_send_queue_create(&pacing, &stream->queue);
  if (status != LS200_STATUS_OK) return status;
  (void)memset(&reporter_config, 0, sizeof(reporter_config));
  reporter_config.local_ssrc = stream->identity.ssrc; reporter_config.local_cname = session->config.local_cname;
  reporter_config.report_interval_ms = session->config.report_interval_ms;
  reporter_config.feedback_media_ssrc = stream->identity.ssrc;
  reporter_config.feedback_policy.accept_pli = 1; reporter_config.feedback_policy.accept_fir = 1;
  reporter_config.feedback_policy.emit_pli = 1; reporter_config.feedback_policy.emit_fir = 1;
  reporter_config.feedback_minimum_interval_ms = LS200_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS;
  return ls200_rtcp_reporter_create(&reporter_config, &stream->reporter);
}

int ls200_media_session_h264_profile_matches(const ls200_media_session *session) {
  if (session == NULL || session->negotiated_h264_profile_level_id[0] == '\0' ||
      session->status.active_h264_profile_level_id[0] == '\0') {
    return 0;
  }
  return ls200_h264_profile_level_id_compatible(
      session->negotiated_h264_profile_level_id,
      session->status.active_h264_profile_level_id);
}

ls200_status ls200_media_session_reserve_internal(
    ls200_media_session *session,
    const ls200_sdp_local_capabilities *requested_capabilities,
    ls200_sdp_local_capabilities *out_capabilities) {
  ls200_rtp_transport_info video_transport;
  ls200_rtp_transport_info audio_transport;
  ls200_status status;
  if (session == NULL || requested_capabilities == NULL || out_capabilities == NULL ||
      session->state != LS200_MEDIA_SESSION_NEW || requested_capabilities->local_address == NULL ||
      strcmp(requested_capabilities->local_address, session->config.advertised_address) != 0) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  session->state = LS200_MEDIA_SESSION_PREPARING;
  session->status.state = session->state;
  session->video.receive_media_id = 1U;
  session->audio.receive_media_id = 2U;
  status = media_session_select_reserved_video(&requested_capabilities->video,
                                               &session->video_payload_type);
  if (status == LS200_STATUS_OK) {
    status = media_session_select_reserved_audio(&requested_capabilities->audio,
                                                 &session->audio_payload_type, &session->use_pcma);
  }
  if (status == LS200_STATUS_OK) {
    status = media_session_create_stream(session, &session->video,
                                         &requested_capabilities->video,
                                         session->video_payload_type,
                                         LS200_MEDIA_SESSION_VIDEO_PACE_NS,
                                         session->config.maximum_video_queue_packets);
  }
  if (status == LS200_STATUS_OK) {
    status = media_session_create_stream(session, &session->audio,
                                         &requested_capabilities->audio,
                                         session->audio_payload_type,
                                         LS200_MEDIA_SESSION_AUDIO_PACE_NS,
                                         session->config.maximum_audio_queue_packets);
  }
  if (status == LS200_STATUS_OK) status = ls200_rtp_session_get_transport(session->video.transport,
                                                                            &video_transport);
  if (status == LS200_STATUS_OK) status = ls200_rtp_session_get_transport(session->audio.transport,
                                                                            &audio_transport);
  if (status != LS200_STATUS_OK) {
    ls200_media_session_record_error(session, status);
    ls200_media_session_release_prepared(session, 0);
    session->state = LS200_MEDIA_SESSION_NEW;
    session->status.state = session->state;
    return status;
  }
  *out_capabilities = *requested_capabilities;
  out_capabilities->video.ports.rtp_port = video_transport.ports.rtp_port; out_capabilities->video.ports.rtcp_port = video_transport.ports.rtcp_port;
  out_capabilities->video.ports.rtcp_mux = video_transport.ports.rtcp_mux;
  out_capabilities->audio.ports.rtp_port = audio_transport.ports.rtp_port; out_capabilities->audio.ports.rtcp_port = audio_transport.ports.rtcp_port;
  out_capabilities->audio.ports.rtcp_mux = audio_transport.ports.rtcp_mux;
  session->video_direction = requested_capabilities->video.direction; session->audio_direction = requested_capabilities->audio.direction;
  session->state = LS200_MEDIA_SESSION_RESERVED;
  session->status.state = session->state;
  session->status.video_direction = session->video_direction; session->status.audio_direction = session->audio_direction;
  session->status.video_payload_type = session->video_payload_type;
  session->status.audio_payload_type = session->audio_payload_type;
  return LS200_STATUS_OK;
}

static ls200_status media_session_prepare_validate_transport(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio) {
  ls200_rtp_transport_info video_transport;
  ls200_rtp_transport_info audio_transport;
  ls200_status status = ls200_rtp_session_get_transport(session->video.transport,
                                                         &video_transport);
  if (status == LS200_STATUS_OK) {
    status = ls200_rtp_session_get_transport(session->audio.transport, &audio_transport);
  }
  if (status == LS200_STATUS_OK &&
      (video->local_rtp_port != video_transport.ports.rtp_port ||
       video->local_rtcp_port != video_transport.ports.rtcp_port ||
       video->local_rtcp_mux != video_transport.ports.rtcp_mux ||
       audio->local_rtp_port != audio_transport.ports.rtp_port ||
       audio->local_rtcp_port != audio_transport.ports.rtcp_port ||
       audio->local_rtcp_mux != audio_transport.ports.rtcp_mux ||
       !media_session_payload_was_reserved(
           &session->video, session->video_receive_payload_type) ||
       !media_session_payload_was_reserved(
           &session->audio, session->audio_receive_payload_type))) {
    status = LS200_STATUS_INVALID_DATA;
  }
  return status;
}

static ls200_status media_session_prepare_collect_payloads(
    ls200_media_session *session, const ls200_sdp_receive_media *video,
    const ls200_sdp_receive_media *audio) {
  ls200_status status = ls200_media_session_collect_negotiated_payloads(
      video->codecs, video->codec_count, &session->video);
  if (status == LS200_STATUS_OK) {
    status = ls200_media_session_collect_negotiated_payloads(audio->codecs, audio->codec_count,
                                                        &session->audio);
  }
  return status;
}

static ls200_status media_session_configure_directional_payloads(
    ls200_media_session_stream *stream,
    const ls200_sdp_negotiated_media *outbound) {
  uint8_t outbound_types[LS200_SDP_MAX_SELECTED_CODECS];
  ls200_rtp_payload_set inbound_set;
  ls200_rtp_payload_set outbound_set;
  size_t index;
  for (index = 0U; index < outbound->codec_count; ++index)
    outbound_types[index] = outbound->codecs[index].payload_type;
  inbound_set.payload_types = stream->payload_types;
  inbound_set.payload_type_count = stream->payload_type_count;
  outbound_set.payload_types = outbound_types;
  outbound_set.payload_type_count = outbound->codec_count;
  return ls200_rtp_session_configure_directional_payloads(
      stream->transport, &inbound_set, &outbound_set);
}

static ls200_status media_session_prepare_authorize_stream(
    const ls200_sdp_negotiated_media *media, ls200_media_session_stream *stream) {
  ls200_rtp_remote_target_authorization target_authorization;
  ls200_status status;
  (void)memset(&target_authorization, 0, sizeof(target_authorization));
  status = media_session_remote_target(media, &target_authorization.approved_remote);
  target_authorization.authorization_epoch = 1U;
  target_authorization.reason_code = "negotiated_sdp";
  if (status == LS200_STATUS_OK) {
    status = ls200_rtp_session_authorize_remote_target(stream->transport,
                                                        &target_authorization);
  }
  return status;
}

static ls200_status media_session_prepare_srtp_stream(
    const ls200_sdp_negotiated_media *media,
    const ls200_sdp_srtp_lifetimes *lifetimes,
    ls200_media_session_stream *stream) {
  ls200_rtp_srtp_config config;
  ls200_rtp_srtp_limits limits;
  ls200_status status;
  if (media == NULL || lifetimes == NULL || stream == NULL || stream->transport == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (media->srtp_enabled == 0) return LS200_STATUS_OK;
  (void)memcpy(config.local_outbound_key_salt, media->local_outbound_srtp.master_key_salt,
               sizeof(config.local_outbound_key_salt));
  (void)memcpy(config.remote_inbound_key_salt, media->remote_inbound_srtp.master_key_salt,
               sizeof(config.remote_inbound_key_salt));
  limits.local_outbound = lifetimes->local_outbound;
  limits.remote_inbound = lifetimes->remote_inbound;
  status = ls200_rtp_session_configure_srtp_with_limits(stream->transport,
                                                         &config, &limits);
  media_session_secure_zero(&config, sizeof(config));
  return status;
}

static ls200_status media_session_prepare_srtp(
    ls200_media_session *session, const ls200_sdp_negotiated_session *negotiated,
    const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio) {
  ls200_sdp_srtp_lifetimes video_lifetimes;
  ls200_sdp_srtp_lifetimes audio_lifetimes;
  ls200_status status = ls200_sdp_negotiated_get_srtp_lifetimes(
      negotiated, 1, &video_lifetimes);
  if (status == LS200_STATUS_OK)
    status = ls200_sdp_negotiated_get_srtp_lifetimes(
        negotiated, 0, &audio_lifetimes);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_srtp_stream(video, &video_lifetimes,
                                                &session->video);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_srtp_stream(audio, &audio_lifetimes,
                                                &session->audio);
  return status;
}

static ls200_status media_session_prepare_open_backend(ls200_media_session *session) {
  ls200_status status = session->config.backend.vtable->open(&session->config.backend,
                                                               session->config.backend_config);
  if (status == LS200_STATUS_OK) session->backend_open = 1;
  return status;
}

static ls200_status media_session_prepare_runtimes(ls200_media_session *session) {
  ls200_status status = media_session_prepare_stream_runtime(
      session, &session->video, LS200_MEDIA_SESSION_VIDEO_PACE_NS,
      session->config.maximum_video_queue_packets);
  if (status == LS200_STATUS_OK) {
    status = media_session_prepare_stream_runtime(
        session, &session->audio, LS200_MEDIA_SESSION_AUDIO_PACE_NS,
        session->config.maximum_audio_queue_packets);
  }
  return status;
}

static ls200_status media_session_prepare_aec(ls200_media_session *session) {
  return ls200_aec_create(&session->config.aec, &session->aec);
}

static ls200_status media_session_prepare_dtmf(ls200_media_session *session) {
  ls200_dtmf_sender_config sender_config; ls200_dtmf_receiver_config receiver_config;
  ls200_rtp_pacing_config pacing; ls200_status status;
  if (session->dtmf_negotiated == 0) return LS200_STATUS_OK;
  (void)memset(&sender_config, 0, sizeof(sender_config));
  sender_config.payload_type = session->dtmf_payload_type; sender_config.ssrc = session->audio.identity.ssrc;
  sender_config.initial_sequence_number = session->audio.identity.initial_sequence_number;
  sender_config.maximum_duration_samples = 60000U; sender_config.end_packet_repetitions = 3U;
  status = ls200_dtmf_sender_create(&sender_config, &session->dtmf_sender);
  pacing.packet_interval_ns = LS200_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS; pacing.maximum_queue_packets = session->config.maximum_audio_queue_packets;
  pacing.maximum_queue_bytes = session->config.maximum_queue_bytes;
  if (status == LS200_STATUS_OK) status = ls200_rtp_send_queue_create(&pacing, &session->audio.dtmf_queue);
  if (status == LS200_STATUS_OK) {
    receiver_config.duplicate_history_entries = 64U; receiver_config.maximum_duration_samples = 60000U;
    status = ls200_dtmf_receiver_create(&receiver_config, &session->dtmf_receiver);
  }
  if (status == LS200_STATUS_OK) session->dtmf_enabled = 1;
  return status;
}

static ls200_status media_session_prepare_receive_shim(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio) {
  uint8_t accepted[LS200_MEDIA_SESSION_MAX_CODECS];
  size_t accepted_count = 0U;
  ls200_rx_shim_config shim_config;
  size_t index;
  (void)video;
  (void)audio;
  for (index = 0U; index < session->video.payload_type_count; ++index) {
    accepted[accepted_count++] = session->video.payload_types[index];
  }
  for (index = 0U; index < session->audio.payload_type_count; ++index) {
    accepted[accepted_count++] = session->audio.payload_types[index];
  }
  (void)memset(&shim_config, 0, sizeof(shim_config));
  shim_config.accepted_payload_types = accepted; shim_config.accepted_payload_type_count = accepted_count;
  shim_config.maximum_packet_bytes = session->config.rtp_mtu; shim_config.maximum_ssrcs = session->config.maximum_ssrcs;
  shim_config.local_ssrc = session->video.identity.ssrc; shim_config.feedback_video_ssrc = session->video.identity.ssrc;
  shim_config.local_cname = session->config.local_cname; shim_config.report_interval_ms = session->config.report_interval_ms;
  shim_config.symmetric_rtp_enabled = session->config.symmetric_rtp_enabled != 0;
  return ls200_rx_shim_create(&shim_config, &session->receive_shim);
}

static ls200_status media_session_prepare_security(
    ls200_media_session *session, const ls200_sdp_negotiated_session *negotiated,
    const ls200_sdp_negotiated_media *video, const ls200_sdp_negotiated_media *audio) {
  ls200_status status = media_session_prepare_authorize_stream(video, &session->video);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_authorize_stream(audio, &session->audio);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_srtp(session, negotiated, video, audio);
  if (status == LS200_STATUS_OK)
    status = ls200_media_session_prepare_feedback_internal(session, negotiated);
  return status;
}

static ls200_status media_session_prepare_components(
    ls200_media_session *session,
    const ls200_sdp_negotiated_session *negotiated,
    const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio) {
  const ls200_sdp_receive_media *receive_video =
      ls200_sdp_negotiated_get_receive_media(negotiated, 1);
  const ls200_sdp_receive_media *receive_audio =
      ls200_sdp_negotiated_get_receive_media(negotiated, 0);
  ls200_status status = receive_video == NULL || receive_audio == NULL ?
      LS200_STATUS_INVALID_DATA : ls200_media_session_prepare_select_codecs_internal(
          session, video, receive_video, audio, receive_audio);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_validate_transport(session, video, audio);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_collect_payloads(
        session, receive_video, receive_audio);
  if (status == LS200_STATUS_OK)
    status = media_session_configure_directional_payloads(&session->video, video);
  if (status == LS200_STATUS_OK)
    status = media_session_configure_directional_payloads(&session->audio, audio);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_security(session, negotiated, video, audio);
  if (status == LS200_STATUS_OK) status = media_session_prepare_open_backend(session);
  if (status == LS200_STATUS_OK) status = media_session_prepare_runtimes(session);
  if (status == LS200_STATUS_OK) status = media_session_prepare_aec(session);
  if (status == LS200_STATUS_OK) status = media_session_prepare_dtmf(session);
  if (status == LS200_STATUS_OK)
    status = media_session_prepare_receive_shim(session, video, audio);
  return status;
}

ls200_status ls200_media_session_prepare_internal(ls200_media_session *session,
                                         const ls200_sdp_negotiated_session *negotiated) {
  const ls200_sdp_negotiated_media *video;
  const ls200_sdp_negotiated_media *audio;
  ls200_status status;
  if (session == NULL || negotiated == NULL || session->state != LS200_MEDIA_SESSION_RESERVED) {
    return LS200_STATUS_STATE_ERROR;
  }
  video = ls200_sdp_negotiated_get_video(negotiated);
  audio = ls200_sdp_negotiated_get_audio(negotiated);
  if (video == NULL || audio == NULL) return LS200_STATUS_INVALID_DATA;
  session->state = LS200_MEDIA_SESSION_PREPARING;
  session->status.state = session->state;
  session->status.video_srtp = 0;
  session->status.audio_srtp = 0;
  status = media_session_prepare_components(session, negotiated, video, audio);
  if (status != LS200_STATUS_OK) {
    ls200_media_session_record_error(session, status);
    ls200_media_session_release_prepared(session, 0);
    session->state = LS200_MEDIA_SESSION_NEW;
    session->status.state = session->state;
    return status;
  }
  ls200_media_session_prepare_finalize_internal(session, video, audio);
  return LS200_STATUS_OK;
}
