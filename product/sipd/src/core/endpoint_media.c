#include "endpoint_internal.h"
#include "endpoint_media_policy.h"
#include "ls200_sipd/platform.h"
#include "../sip/pjsip_readiness.h"
#include "../sdp/negotiated_private.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ls200_sdp_codec AUDIO_CODECS[] = {
  {LS200_SDP_CODEC_PCMU, 0U, 8000U, 1U, NULL},
  {LS200_SDP_CODEC_PCMA, 8U, 8000U, 1U, NULL},
  {LS200_SDP_CODEC_TELEPHONE_EVENT, 101U, 8000U, 1U, "0-16"}
};

static void endpoint_secure_zero(void *memory, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

static int endpoint_initial_offer_uses_srtp(const ls200_endpoint *endpoint) {
  return endpoint != NULL && endpoint->view != NULL &&
      endpoint->view->media_security_policy != PLAIN_COMPAT;
}

static int endpoint_media_uses_srtp(const ls200_sdp_media *media) {
  return media != NULL && media->profile == LS200_SDP_MEDIA_PROFILE_RTP_SAVP;
}

static ls200_status endpoint_committed_media_security(
    const ls200_endpoint *endpoint, int *out_video_srtp, int *out_audio_srtp) {
  ls200_media_session_status active;
  ls200_status status;
  if (endpoint == NULL || out_video_srtp == NULL || out_audio_srtp == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (endpoint->media == NULL) return LS200_STATUS_STATE_ERROR;
  status = ls200_media_session_get_status(endpoint->media, &active);
  if (status != LS200_STATUS_OK) return status;
  if (active.state != LS200_MEDIA_SESSION_COMMITTED) return LS200_STATUS_STATE_ERROR;
  *out_video_srtp = active.video_srtp;
  *out_audio_srtp = active.audio_srtp;
  return LS200_STATUS_OK;
}

static int endpoint_reinvite_policy_arguments_are_valid(
    const ls200_endpoint *endpoint, const ls200_sdp_media *video,
    const ls200_sdp_media *audio) {
  return endpoint != NULL && endpoint->view != NULL && video != NULL && audio != NULL;
}

static ls200_status endpoint_generate_srtp_policies(
    ls200_sdp_srtp_policy *video, ls200_sdp_srtp_policy *audio) {
  ls200_mutable_bytes video_output;
  ls200_mutable_bytes audio_output;
  ls200_status status;
  if (video == NULL || audio == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(video, 0, sizeof(*video));
  (void)memset(audio, 0, sizeof(*audio));
  video->crypto_tag = 1U;
  audio->crypto_tag = 1U;
  video_output.data = video->outbound_material.master_key_salt;
  video_output.capacity = sizeof(video->outbound_material.master_key_salt);
  video_output.length = 0U;
  audio_output.data = audio->outbound_material.master_key_salt;
  audio_output.capacity = sizeof(audio->outbound_material.master_key_salt);
  audio_output.length = 0U;
  status = ls200_platform_random_bytes(&video_output);
  if (status == LS200_STATUS_OK && video_output.length != video_output.capacity) {
    status = LS200_STATUS_IO_ERROR;
  }
  if (status == LS200_STATUS_OK) status = ls200_platform_random_bytes(&audio_output);
  if (status == LS200_STATUS_OK && audio_output.length != audio_output.capacity) {
    status = LS200_STATUS_IO_ERROR;
  }
  if (status == LS200_STATUS_OK &&
      memcmp(video->outbound_material.master_key_salt,
             audio->outbound_material.master_key_salt,
             LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES) == 0) {
    status = LS200_STATUS_INTERNAL_ERROR;
  }
  if (status != LS200_STATUS_OK) {
    endpoint_secure_zero(video, sizeof(*video));
    endpoint_secure_zero(audio, sizeof(*audio));
  }
  return status;
}

static ls200_status endpoint_reinvite_policy_allows(
    const ls200_endpoint *endpoint, const ls200_sdp_media *video,
    const ls200_sdp_media *audio) {
  int active_video_srtp;
  int active_audio_srtp;
  int video_srtp;
  int audio_srtp;
  ls200_status status;
  if (!endpoint_reinvite_policy_arguments_are_valid(endpoint, video, audio)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  video_srtp = endpoint_media_uses_srtp(video);
  audio_srtp = endpoint_media_uses_srtp(audio);
  status = endpoint_committed_media_security(endpoint, &active_video_srtp,
                                              &active_audio_srtp);
  if (status != LS200_STATUS_OK) return status;
  if (endpoint->view->media_security_policy == REQUIRE_SRTP &&
      (!video_srtp || !audio_srtp)) {
    return LS200_STATUS_UNSUPPORTED;
  }
  if (endpoint->view->media_security_policy == PLAIN_COMPAT &&
      (video_srtp || audio_srtp)) {
    return LS200_STATUS_UNSUPPORTED;
  }
  if ((active_video_srtp != 0 && !video_srtp) ||
      (active_audio_srtp != 0 && !audio_srtp)) {
    return LS200_STATUS_UNSUPPORTED;
  }
  return LS200_STATUS_OK;
}

static int endpoint_peer_is_restricted(const ls200_sip_transport_info *peer) {
  return ls200_sip_transport_is_restricted_network(peer);
}

static int endpoint_fixture_peer_is_allowed(const ls200_endpoint *endpoint,
                                            const ls200_sip_transport_info *peer) {
  return endpoint->backend_config.name != NULL &&
      strcmp(endpoint->backend_config.name, "fixture") == 0 &&
      peer->remote_address_length == 4U && peer->remote_address[0] == 127U;
}

static int endpoint_private_literal_matches(const ls200_endpoint *endpoint,
                                            const ls200_sip_transport_info *peer) {
  const char *uri = endpoint->sip_config.outbound_uri;
  const char *host;
  const char *host_end;
  uint8_t literal[4];
  ls200_sip_dial_target target;
  if (uri == NULL || ls200_sip_parse_dial_target(uri, &target) != LS200_STATUS_OK ||
      target.port != peer->remote_port) return 0;
  host = strchr(uri + 4U, '@');
  if (host == NULL) return 0;
  ++host;
  host_end = host + strcspn(host, ":;");
  if ((size_t)(host_end - host) >= INET_ADDRSTRLEN) return 0;
  {
    char text[INET_ADDRSTRLEN];
    (void)memcpy(text, host, (size_t)(host_end - host));
    text[host_end - host] = '\0';
    if (inet_pton(AF_INET, text, literal) != 1) return 0;
  }
  return peer->remote_address_length == sizeof(literal) &&
      memcmp(peer->remote_address, literal, sizeof(literal)) == 0;
}

static int endpoint_transport_policy_allows(const ls200_endpoint *endpoint,
                                            const ls200_sip_transport_info *peer,
                                            int fallback) {
  if (fallback == 0) return peer->transport == endpoint->view->transport;
  return endpoint->view->transport == LS200_TRANSPORT_UDP &&
      peer->transport == LS200_TRANSPORT_TCP;
}

static int endpoint_network_policy_allows(const ls200_endpoint *endpoint,
                                          const ls200_sip_transport_info *peer) {
  if (!endpoint_peer_is_restricted(peer)) return 1;
  if (endpoint_fixture_peer_is_allowed(endpoint, peer)) return 1;
  return endpoint_private_literal_matches(endpoint, peer);
}

ls200_status endpoint_scope(void *context, const ls200_sip_transport_info *peer,
                            int fallback) {
  ls200_endpoint *endpoint = (ls200_endpoint *)context;
  if (endpoint == NULL || peer == NULL || endpoint->view == NULL ||
      endpoint->view->enable_public_network == 0) {
    return LS200_STATUS_PERMISSION_DENIED;
  }
  if (!endpoint_transport_policy_allows(endpoint, peer, fallback) ||
      (peer->transport == LS200_TRANSPORT_TLS && endpoint->view->enable_tls == 0)) {
    return LS200_STATUS_PERMISSION_DENIED;
  }
  if (!ls200_sip_transport_is_valid(peer) ||
      !endpoint_network_policy_allows(endpoint, peer)) {
    return LS200_STATUS_SECURITY_ERROR;
  }
  endpoint->authorized_sip_peer = *peer;
  endpoint->has_authorized_sip_peer = 1;
  return LS200_STATUS_OK;
}

static ls200_status endpoint_media_scope(void *context, const char *address,
                                         uint16_t port, int is_rtcp) {
  ls200_endpoint *endpoint = (ls200_endpoint *)context;
  uint8_t parsed[4];
  if (endpoint == NULL || address == NULL || port == 0U ||
      endpoint->has_authorized_sip_peer == 0 ||
      !ls200_sip_transport_is_valid(&endpoint->authorized_sip_peer)) {
    if (endpoint != NULL)
      endpoint_log(endpoint, LS200_LOG_WARNING, "media_target_rejected",
                   "sip_peer_not_pinned");
    return LS200_STATUS_SECURITY_ERROR;
  }
  if (endpoint->media_target_authorizer != NULL) {
    ls200_status status = endpoint->media_target_authorizer(
        endpoint->media_target_context, address, port, is_rtcp);
    if (status != LS200_STATUS_OK)
      endpoint_log(endpoint, LS200_LOG_WARNING, "media_target_rejected",
                   "external_policy_rejected");
    return status;
  }
  if (inet_pton(AF_INET, address, parsed) != 1 ||
      !endpoint_media_policy_allows(endpoint, parsed, port)) {
    endpoint_log(endpoint, LS200_LOG_WARNING, "media_target_rejected",
                 "sip_peer_address_mismatch");
    return LS200_STATUS_SECURITY_ERROR;
  }
  return LS200_STATUS_OK;
}

void endpoint_destroy_media_session(ls200_media_session **session) {
  if (session == NULL || *session == NULL) return;
  (void)ls200_media_session_stop(*session);
  ls200_media_session_destroy(*session);
  *session = NULL;
}

void endpoint_release_pending_media(ls200_endpoint *endpoint) {
  if (endpoint == NULL) return;
  ls200_sdp_negotiated_session_destroy(endpoint->staged_reinvite);
  endpoint->staged_reinvite = NULL;
  if (endpoint->pending_media != NULL) {
    ls200_media_session_rollback(endpoint->pending_media);
  }
  endpoint_destroy_media_session(&endpoint->pending_media);
  (void)memset(&endpoint->pending_reserved, 0,
               sizeof(endpoint->pending_reserved));
  endpoint->reinvite_pending = 0;
  endpoint->reinvite_cseq = 0U;
}

void endpoint_release_media(ls200_endpoint *endpoint) {
  if (endpoint == NULL) return;
  endpoint_clear_dtmf_schedule(endpoint);
  ls200_pjsip_media_readiness_cancel();
  ls200_sdp_session_destroy(endpoint->offer);
  endpoint->offer = NULL;
  endpoint_report_media_totals(endpoint->media);
  endpoint_destroy_media_session(&endpoint->media);
  (void)memset(&endpoint->reserved, 0, sizeof(endpoint->reserved));
}

void endpoint_retire_dialog(ls200_endpoint *endpoint) {
  if (endpoint == NULL) return;
  ls200_sip_dialog_destroy(endpoint->dialog);
  endpoint->dialog = NULL;
  endpoint->invite_deadline_ns = 0U;
}

static ls200_status endpoint_requested_capabilities(
    ls200_endpoint *endpoint, ls200_sdp_codec *dynamic_video_codec,
    char *dynamic_video_fmtp, size_t dynamic_video_fmtp_capacity,
    ls200_sdp_local_capabilities *requested) {
  ls200_sdp_local_capabilities ungated;
  size_t video_codec_count;
  if (endpoint == NULL || requested == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&ungated, 0, sizeof(ungated));
  ungated.local_address = endpoint->addresses.advertised_address;
  ungated.remote_target_authorizer = endpoint_media_scope;
  ungated.remote_target_context = endpoint;
  ungated.video.direction = LS200_SDP_SENDRECV;
  ungated.video.codecs = endpoint_video_codecs(
      endpoint, dynamic_video_codec, dynamic_video_fmtp,
      dynamic_video_fmtp_capacity, &video_codec_count);
  if (ungated.video.codecs == NULL) return LS200_STATUS_STATE_ERROR;
  ungated.video.codec_count = video_codec_count;
  ungated.audio.direction = LS200_SDP_SENDRECV;
  ungated.audio.codecs = AUDIO_CODECS;
  ungated.audio.codec_count = sizeof(AUDIO_CODECS) / sizeof(AUDIO_CODECS[0]);
  return ls200_media_session_gate_renderer(endpoint->renderer, &ungated, requested);
}

ls200_status endpoint_prepare_offer(ls200_endpoint *endpoint,
                                    ls200_mutable_bytes *serialized) {
  ls200_sdp_local_capabilities requested;
  ls200_sdp_srtp_policy video_srtp;
  ls200_sdp_srtp_policy audio_srtp;
  ls200_sdp_codec dynamic_video_codec;
  char dynamic_video_fmtp[64];
  ls200_status status;
  (void)memset(&video_srtp, 0, sizeof(video_srtp));
  (void)memset(&audio_srtp, 0, sizeof(audio_srtp));
  if (endpoint == NULL || serialized == NULL || endpoint->media != NULL) {
    return LS200_STATUS_STATE_ERROR;
  }
  (void)memset(&dynamic_video_codec, 0, sizeof(dynamic_video_codec));
  (void)memset(dynamic_video_fmtp, 0, sizeof(dynamic_video_fmtp));
  status = endpoint_discover_source_h264(endpoint);
  if (status == LS200_STATUS_OK)
    status = endpoint_create_media_session(endpoint, &endpoint->media);
  if (status == LS200_STATUS_OK) {
    status = endpoint_requested_capabilities(
        endpoint, &dynamic_video_codec, dynamic_video_fmtp,
        sizeof(dynamic_video_fmtp), &requested);
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_media_session_reserve(endpoint->media, &requested,
                                         &endpoint->reserved);
  }
  if (status == LS200_STATUS_OK) {
    if (endpoint_initial_offer_uses_srtp(endpoint)) {
      status = endpoint_generate_srtp_policies(&video_srtp, &audio_srtp);
      if (status == LS200_STATUS_OK) {
        status = ls200_sdp_create_initial_srtp_offer(&endpoint->reserved,
            &video_srtp, &audio_srtp, &endpoint->offer);
      }
    } else {
      status = ls200_sdp_create_initial_offer(&endpoint->reserved, &endpoint->offer);
    }
  }
  if (status == LS200_STATUS_OK)
    status = ls200_sdp_offer_enable_video_feedback(endpoint->offer,
        LS200_SDP_FEEDBACK_NACK | LS200_SDP_FEEDBACK_PLI | LS200_SDP_FEEDBACK_FIR);
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_serialize(endpoint->offer, serialized);
  }
  endpoint_secure_zero(&video_srtp, sizeof(video_srtp));
  endpoint_secure_zero(&audio_srtp, sizeof(audio_srtp));
  if (status != LS200_STATUS_OK) endpoint_release_media(endpoint);
  return status;
}

void endpoint_retry(ls200_endpoint *endpoint, uint32_t retry_after_seconds) {
  uint8_t random_byte = 0U;
  ls200_mutable_bytes random = {&random_byte, 1U, 0U};
  uint64_t now;
  uint32_t delay;
  uint32_t retry_ms = retry_after_seconds > 30U ? 30000U :
      retry_after_seconds * 1000U;
  if (endpoint == NULL) return;
  endpoint->invite_deadline_ns = 0U;
  if (ls200_call_apply_event(endpoint->call, LS200_CALL_EVENT_TRANSIENT_FAILURE,
                             NULL) != LS200_STATUS_OK) return;
  endpoint_release_pending_media(endpoint);
  endpoint_release_media(endpoint);
  endpoint_retire_dialog(endpoint);
  if (ls200_platform_random_bytes(&random) != LS200_STATUS_OK ||
      ls200_call_schedule_reconnect_with_retry_after(endpoint->call, random_byte,
          retry_ms, &delay) != LS200_STATUS_OK ||
      ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) {
    ls200_sip_adapter_clear_outbound_target(endpoint->adapter);
    return;
  }
  endpoint->retry_deadline_ns = endpoint_deadline_after(
      now, (uint64_t)delay * UINT64_C(1000000));
}

void endpoint_media_failure(ls200_endpoint *endpoint) {
  ls200_status bye_status = LS200_STATUS_STATE_ERROR;
  if (endpoint == NULL) return;
  if (endpoint->dialog != NULL) {
    bye_status = ls200_sip_adapter_send_bye(endpoint->dialog);
  }
  endpoint_release_pending_media(endpoint);
  if (endpoint->media != NULL) ls200_media_session_rollback(endpoint->media);
  endpoint_release_media(endpoint);
  (void)ls200_call_apply_event(endpoint->call, LS200_CALL_EVENT_MEDIA_ERROR, NULL);
  if (bye_status != LS200_STATUS_OK) endpoint_retry(endpoint, 0U);
}

static ls200_status endpoint_stage_pending_media(
    ls200_endpoint *endpoint, ls200_sdp_local_capabilities *requested) {
  ls200_sdp_codec dynamic_video_codec;
  char dynamic_video_fmtp[64];
  ls200_status status = endpoint_create_media_session(endpoint, &endpoint->pending_media);
  (void)memset(&dynamic_video_codec, 0, sizeof(dynamic_video_codec));
  (void)memset(dynamic_video_fmtp, 0, sizeof(dynamic_video_fmtp));
  if (status == LS200_STATUS_OK)
    status = endpoint_requested_capabilities(
        endpoint, &dynamic_video_codec, dynamic_video_fmtp,
        sizeof(dynamic_video_fmtp), requested);
  if (status == LS200_STATUS_OK)
    status = ls200_media_session_reserve(endpoint->pending_media, requested,
                                         &endpoint->pending_reserved);
  return status;
}

static int endpoint_direction_sends(ls200_sdp_direction direction) {
  return direction == LS200_SDP_SENDRECV || direction == LS200_SDP_SENDONLY;
}

static int endpoint_direction_receives(ls200_sdp_direction direction) {
  return direction == LS200_SDP_SENDRECV || direction == LS200_SDP_RECVONLY;
}

static ls200_sdp_direction endpoint_reinvite_answer_direction(
    ls200_sdp_direction local_capability, ls200_sdp_direction remote_offer) {
  int sends = endpoint_direction_sends(local_capability) &&
      endpoint_direction_receives(remote_offer);
  int receives = endpoint_direction_receives(local_capability) &&
      endpoint_direction_sends(remote_offer);
  if (sends && receives) return LS200_SDP_SENDRECV;
  if (sends) return LS200_SDP_SENDONLY;
  if (receives) return LS200_SDP_RECVONLY;
  return LS200_SDP_INACTIVE;
}

static void endpoint_build_reinvite_policy(
    ls200_endpoint *endpoint, const ls200_sdp_session *offer,
    ls200_sdp_answer_policy *policy) {
  const ls200_sdp_media *video_offer = ls200_sdp_get_video(offer);
  const ls200_sdp_media *audio_offer = ls200_sdp_get_audio(offer);
  (void)memset(policy, 0, sizeof(*policy));
  policy->local_address = endpoint->addresses.advertised_address;
  policy->video_ports = endpoint->pending_reserved.video.ports;
  policy->audio_ports = endpoint->pending_reserved.audio.ports;
  policy->video_direction = endpoint_reinvite_answer_direction(
      endpoint->pending_reserved.video.direction, video_offer->direction);
  policy->audio_direction = endpoint_reinvite_answer_direction(
      endpoint->pending_reserved.audio.direction, audio_offer->direction);
  policy->allow_pcmu = 1;
  policy->allow_pcma = 1;
  policy->allow_telephone_event = 1;
  policy->allow_h264_packetization_mode_1 = 1;
  policy->remote_target_authorizer = endpoint_media_scope;
  policy->remote_target_context = endpoint;
}

static ls200_status endpoint_answer_reinvite_offer(
    ls200_endpoint *endpoint, const ls200_sip_reinvite *reinvite,
    ls200_sdp_session **out_offer, ls200_sdp_session **out_answer) {
  ls200_sdp_answer_policy policy;
  ls200_sdp_srtp_policy video_srtp;
  ls200_sdp_srtp_policy audio_srtp;
  const ls200_sdp_media *video_offer;
  const ls200_sdp_media *audio_offer;
  int use_srtp;
  ls200_status status;
  (void)memset(&video_srtp, 0, sizeof(video_srtp));
  (void)memset(&audio_srtp, 0, sizeof(audio_srtp));
  status = ls200_sdp_parse_offer(reinvite->offer_sdp, out_offer);
  if (status == LS200_STATUS_OK) {
    endpoint_build_reinvite_policy(endpoint, *out_offer, &policy);
    video_offer = ls200_sdp_get_video(*out_offer);
    audio_offer = ls200_sdp_get_audio(*out_offer);
    status = endpoint_reinvite_policy_allows(endpoint, video_offer, audio_offer);
    use_srtp = endpoint_media_uses_srtp(video_offer) || endpoint_media_uses_srtp(audio_offer);
    if (status == LS200_STATUS_OK && use_srtp)
      status = endpoint_generate_srtp_policies(&video_srtp, &audio_srtp);
    if (status == LS200_STATUS_OK && use_srtp)
      status = ls200_sdp_answer_remote_srtp_offer(
          *out_offer, &policy, endpoint_media_uses_srtp(video_offer) ? &video_srtp : NULL,
          endpoint_media_uses_srtp(audio_offer) ? &audio_srtp : NULL, out_answer,
          &endpoint->staged_reinvite);
    if (status == LS200_STATUS_OK && !use_srtp)
      status = ls200_sdp_answer_remote_offer(*out_offer, &policy, out_answer,
                                             &endpoint->staged_reinvite);
  }
  endpoint_secure_zero(&video_srtp, sizeof(video_srtp));
  endpoint_secure_zero(&audio_srtp, sizeof(audio_srtp));
  return status;
}

static ls200_status endpoint_prepare_and_send_reinvite_answer(
    ls200_endpoint *endpoint, const ls200_sdp_session *answer) {
  uint8_t answer_data[LS200_SIPD_MAX_SDP_BYTES];
  ls200_mutable_bytes answer_bytes = {answer_data, sizeof(answer_data), 0U};
  ls200_status status = ls200_media_session_prepare(endpoint->pending_media,
                                                     endpoint->staged_reinvite);
  if (status == LS200_STATUS_OK) status = ls200_sdp_serialize(answer, &answer_bytes);
  if (status == LS200_STATUS_OK)
    status = ls200_sip_adapter_answer_reinvite(
        endpoint->dialog, (ls200_bytes){answer_bytes.data, answer_bytes.length});
  (void)memset(answer_data, 0, sizeof(answer_data));
  return status;
}

static void endpoint_reject_staged_reinvite(ls200_endpoint *endpoint,
                                            ls200_status status) {
  endpoint_release_pending_media(endpoint);
  (void)ls200_sip_adapter_reject_reinvite(
      endpoint->dialog, status == LS200_STATUS_INVALID_DATA ? 400U : 488U);
}

ls200_status endpoint_stage_reinvite(ls200_endpoint *endpoint,
                                     const ls200_sip_event *event) {
  ls200_sdp_session *offer = NULL;
  ls200_sdp_session *answer = NULL;
  ls200_sdp_local_capabilities requested;
  ls200_status status;
  if (endpoint == NULL || event == NULL || event->reinvite == NULL ||
      endpoint->dialog == NULL) {
    return LS200_STATUS_STATE_ERROR;
  }
  if (endpoint->reinvite_pending != 0)
    return ls200_sip_adapter_reject_reinvite(endpoint->dialog, 491U);
  status = endpoint_stage_pending_media(endpoint, &requested);
  if (status == LS200_STATUS_OK)
    status = endpoint_answer_reinvite_offer(endpoint, event->reinvite, &offer, &answer);
  if (status == LS200_STATUS_OK)
    status = endpoint_prepare_and_send_reinvite_answer(endpoint, answer);
  if (status == LS200_STATUS_OK) {
    endpoint->reinvite_pending = 1;
    endpoint->reinvite_cseq = event->reinvite->request_transaction.cseq_number;
  } else {
    endpoint_reject_staged_reinvite(endpoint, status);
  }
  ls200_sdp_session_destroy(answer);
  ls200_sdp_session_destroy(offer);
  return status;
}

void endpoint_poll_media(ls200_endpoint *endpoint, ls200_deadline deadline) {
  ls200_status status;
  if (endpoint->media == NULL ||
      ls200_call_get_state(endpoint->call) != LS200_CALL_ESTABLISHED) return;
  status = ls200_media_session_poll(endpoint->media, deadline);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN &&
      status != LS200_STATUS_END) {
    (void)fprintf(stderr, "ls200-sipd: media-stage=poll code=%d\n", (int)status);
    (void)fflush(stderr);
    endpoint_media_failure(endpoint);
  }
}
