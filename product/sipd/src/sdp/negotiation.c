#include "sdp_internal.h"
#include "../media/h264_profile_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aula_status aula_sdp_copy_h264_evidence(aula_sdp_h264_evidence *evidence,
                                                  const char *fmtp) {
  char copy[AULA_SDP_MAX_FMTP_BYTES];
  char *field;
  if (evidence == NULL || !aula_sdp_h264_fmtp_is_compatible(fmtp) ||
      strlen(fmtp) >= sizeof(copy)) {
    return AULA_STATUS_INVALID_DATA;
  }
  (void)memcpy(copy, fmtp, strlen(fmtp) + 1U);
  evidence->present = 1;
  evidence->packetization_mode = 1;
  field = strtok(copy, ";");
  while (field != NULL) {
    char *equals;
    while (*field == ' ') {
      ++field;
    }
    equals = strchr(field, '=');
    if (equals == NULL) {
      return AULA_STATUS_INVALID_DATA;
    }
    *equals++ = '\0';
    if (aula_sdp_ascii_equal(field, "profile-level-id")) {
      (void)memcpy(evidence->profile_level_id, equals, 6U);
      evidence->profile_level_id[6] = '\0';
    } else if (aula_sdp_ascii_equal(field, "sprop-parameter-sets")) {
      if (*equals == '\0' || strlen(equals) >= sizeof(evidence->parameter_sets)) {
        return AULA_STATUS_INVALID_DATA;
      }
      (void)memcpy(evidence->parameter_sets, equals, strlen(equals) + 1U);
      evidence->has_parameter_sets = 1;
    }
    field = strtok(NULL, ";");
  }
  return evidence->profile_level_id[0] == '\0' ? AULA_STATUS_INVALID_DATA : AULA_STATUS_OK;
}

static int aula_sdp_codec_matches(const aula_sdp_codec *left,
                                   const aula_sdp_codec *right) {
  aula_sdp_h264_evidence left_h264;
  aula_sdp_h264_evidence right_h264;
  if (left == NULL || right == NULL || left->kind != right->kind ||
      left->clock_rate != right->clock_rate || left->channels != right->channels ||
      left->format_parameters == NULL ||
      right->format_parameters == NULL) {
    return 0;
  }
  if (left->kind == AULA_SDP_CODEC_H264) {
    (void)memset(&left_h264, 0, sizeof(left_h264));
    (void)memset(&right_h264, 0, sizeof(right_h264));
    if (aula_sdp_copy_h264_evidence(&left_h264, left->format_parameters) !=
            AULA_STATUS_OK ||
        aula_sdp_copy_h264_evidence(&right_h264, right->format_parameters) !=
            AULA_STATUS_OK) {
      return 0;
    }
    /* Each sender advertises the SPS/PPS for its own encoded stream.  Those
     * values need not be byte-identical in an answer; the interoperable
     * contract is the selected payload, packetization mode, and profile. */
    return left_h264.packetization_mode == right_h264.packetization_mode &&
           aula_h264_profile_level_id_compatible(
               left_h264.profile_level_id, right_h264.profile_level_id);
  }
  return strcmp(left->format_parameters, right->format_parameters) == 0;
}

static const aula_sdp_codec *aula_sdp_find_matching_codec(
    const aula_sdp_stored_media *media, const aula_sdp_codec *selected) {
  size_t index;
  for (index = 0U; media != NULL && index < media->value.codec_count; ++index) {
    if (aula_sdp_codec_matches(&media->codecs[index], selected))
      return &media->codecs[index];
  }
  return NULL;
}

static aula_status aula_sdp_authorize_negotiated_targets(
    const aula_sdp_session *local, const aula_sdp_session *remote,
    const aula_sdp_stored_media *remote_media, const char **out_rtcp_address) {
  const char *rtcp_address = remote_media->rtcp_address[0] == '\0' ?
                                 remote->connection_address : remote_media->rtcp_address;
  if (aula_sdp_authorize_remote_ipv4(local, remote->connection_address,
                                      remote_media->value.rtp_port, 0) != AULA_STATUS_OK ||
      aula_sdp_authorize_remote_ipv4(local, rtcp_address,
                                      remote_media->value.rtcp_port, 1) != AULA_STATUS_OK)
    return AULA_STATUS_SECURITY_ERROR;
  *out_rtcp_address = rtcp_address;
  return AULA_STATUS_OK;
}

static aula_sdp_direction aula_sdp_effective_local_answer_direction(
    aula_sdp_direction remote_direction) {
  switch (remote_direction) {
    case AULA_SDP_SENDRECV: return AULA_SDP_SENDRECV;
    case AULA_SDP_SENDONLY: return AULA_SDP_RECVONLY;
    case AULA_SDP_RECVONLY: return AULA_SDP_SENDONLY;
    case AULA_SDP_INACTIVE: return AULA_SDP_INACTIVE;
  }
  return AULA_SDP_INACTIVE;
}

static void aula_sdp_copy_negotiated_transport(
    aula_sdp_negotiated_media *target, const aula_sdp_stored_media *local_media,
    const aula_sdp_session *remote, const aula_sdp_stored_media *remote_media,
    const char *rtcp_address, int remote_is_answer) {
  (void)memcpy(target->remote_rtp_address, remote->connection_address,
               strlen(remote->connection_address) + 1U);
  (void)memcpy(target->remote_rtcp_address, rtcp_address, strlen(rtcp_address) + 1U);
  target->local_rtp_port = local_media->value.rtp_port;
  target->local_rtcp_port = local_media->value.rtcp_port;
  target->remote_rtp_port = remote_media->value.rtp_port;
  target->remote_rtcp_port = remote_media->value.rtcp_port;
  target->local_rtcp_mux = local_media->value.rtcp_mux != 0;
  target->remote_rtcp_mux = remote_media->value.rtcp_mux != 0;
  target->local_direction = remote_is_answer != 0 ?
      aula_sdp_effective_local_answer_direction(remote_media->value.direction) :
      local_media->value.direction;
  target->remote_direction = remote_media->value.direction;
}

static aula_status aula_sdp_copy_negotiated_srtp(
    aula_sdp_negotiated_media *target, const aula_sdp_stored_media *local_media,
    const aula_sdp_stored_media *remote_media,
    aula_sdp_srtp_lifetimes *lifetimes) {
  if (target == NULL || local_media == NULL || remote_media == NULL ||
      lifetimes == NULL || local_media->value.profile != remote_media->value.profile) {
    return AULA_STATUS_UNSUPPORTED;
  }
  if (local_media->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP) {
    return !local_media->srtp.present && !remote_media->srtp.present ? AULA_STATUS_OK :
                                                                        AULA_STATUS_INVALID_DATA;
  }
  if (local_media->value.profile != AULA_SDP_MEDIA_PROFILE_RTP_SAVP ||
      !local_media->srtp.present || !remote_media->srtp.present ||
      local_media->srtp.crypto_tag != remote_media->srtp.crypto_tag) {
    return AULA_STATUS_INVALID_DATA;
  }
  target->srtp_enabled = 1;
  target->local_outbound_srtp_tag = local_media->srtp.crypto_tag;
  target->remote_inbound_srtp_tag = remote_media->srtp.crypto_tag;
  lifetimes->local_outbound = local_media->srtp.lifetime;
  lifetimes->remote_inbound = remote_media->srtp.lifetime;
  (void)memcpy(&target->local_outbound_srtp, &local_media->srtp.material,
               sizeof(target->local_outbound_srtp));
  (void)memcpy(&target->remote_inbound_srtp, &remote_media->srtp.material,
               sizeof(target->remote_inbound_srtp));
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_copy_codec_fields(
    aula_sdp_negotiated_codec *codec, const aula_sdp_codec *selected) {
  if (selected->format_parameters == NULL ||
      strlen(selected->format_parameters) >= sizeof(codec->format_parameters))
    return AULA_STATUS_INVALID_DATA;
  codec->kind = selected->kind;
  codec->payload_type = selected->payload_type;
  codec->clock_rate = selected->clock_rate;
  codec->channels = selected->channels;
  (void)memcpy(codec->format_parameters, selected->format_parameters,
               strlen(selected->format_parameters) + 1U);
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_copy_negotiated_codec(
    aula_sdp_negotiated_media *target, size_t index,
    const aula_sdp_codec *selected) {
  aula_status status = aula_sdp_copy_codec_fields(&target->codecs[index], selected);
  if (status != AULA_STATUS_OK) return status;
  return selected->kind == AULA_SDP_CODEC_H264 ?
             aula_sdp_copy_h264_evidence(&target->h264, selected->format_parameters) :
             AULA_STATUS_OK;
}

static aula_status aula_sdp_copy_receive_codec(
    aula_sdp_receive_media *target, size_t index,
    const aula_sdp_codec *selected) {
  return aula_sdp_copy_codec_fields(&target->codecs[index], selected);
}

static aula_status aula_sdp_copy_negotiated_codecs(
    aula_sdp_negotiated_media *target, aula_sdp_receive_media *receive,
    const aula_sdp_stored_media *receive_media,
    const aula_sdp_stored_media *selected_media) {
  size_t index;
  size_t selected_count = 0U;
  for (index = 0U; index < selected_media->value.codec_count; ++index) {
    const aula_sdp_codec *selected = &selected_media->codecs[index];
    const aula_sdp_codec *offered = aula_sdp_find_matching_codec(
        receive_media, selected);
    aula_status status;
    if (offered == NULL) continue;
    if (selected_count >= AULA_SDP_MAX_SELECTED_CODECS)
      return AULA_STATUS_LIMIT_EXCEEDED;
    status = aula_sdp_copy_negotiated_codec(target, selected_count, selected);
    if (status == AULA_STATUS_OK)
      status = aula_sdp_copy_receive_codec(receive, selected_count, offered);
    if (status != AULA_STATUS_OK) return status;
    ++selected_count;
  }
  target->codec_count = selected_count;
  receive->codec_count = selected_count;
  return selected_count != 0U ? AULA_STATUS_OK : AULA_STATUS_UNSUPPORTED;
}

static void aula_sdp_copy_negotiated_feedback(
    const aula_sdp_negotiated_media *target, aula_sdp_receive_media *receive,
    const aula_sdp_stored_media *local, const aula_sdp_stored_media *remote,
    int remote_is_answer) {
  size_t index;
  for (index = 0U; index < target->codec_count; ++index) {
    uint8_t selected = target->codecs[index].payload_type;
    uint8_t offered = receive->codecs[index].payload_type;
    if (target->codecs[index].kind != AULA_SDP_CODEC_H264) continue;
    receive->feedback_mask = aula_sdp_media_feedback(local,
        remote_is_answer != 0 ? offered : selected) &
        aula_sdp_media_feedback(remote, remote_is_answer != 0 ? selected : offered);
    return;
  }
}

static aula_status aula_sdp_fill_negotiated_media(
    aula_sdp_negotiated_media *target, aula_sdp_receive_media *receive,
    const aula_sdp_session *local,
    const aula_sdp_stored_media *local_media, const aula_sdp_session *remote,
    const aula_sdp_stored_media *remote_media, const aula_sdp_stored_media *selected_media,
    int remote_is_answer,
    aula_sdp_srtp_lifetimes *lifetimes) {
  const char *rtcp_address;
  aula_status status;
  if (target == NULL || local_media == NULL || remote == NULL || remote_media == NULL ||
      selected_media == NULL ||
      selected_media->value.codec_count == 0U ||
      selected_media->value.codec_count > AULA_SDP_MAX_SELECTED_CODECS) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (aula_sdp_authorize_negotiated_targets(local, remote, remote_media,
                                             &rtcp_address) != AULA_STATUS_OK)
    return AULA_STATUS_SECURITY_ERROR;
  aula_sdp_copy_negotiated_transport(target, local_media, remote, remote_media,
                                      rtcp_address, remote_is_answer);
  if (aula_sdp_copy_negotiated_srtp(target, local_media, remote_media,
                                     lifetimes) != AULA_STATUS_OK) {
    return AULA_STATUS_UNSUPPORTED;
  }
  status = aula_sdp_copy_negotiated_codecs(
      target, receive, remote_is_answer != 0 ? local_media : remote_media,
      selected_media);
  if (status == AULA_STATUS_OK)
    aula_sdp_copy_negotiated_feedback(target, receive, local_media, remote_media,
                                       remote_is_answer);
  return status;
}

static int aula_sdp_direct_crc_sendonly_answer(
    const aula_sdp_session *local_offer,
    const aula_sdp_session *remote_answer) {
  return local_offer->video.value.direction == AULA_SDP_SENDONLY &&
      local_offer->audio.value.direction == AULA_SDP_SENDONLY &&
      remote_answer->video.value.direction == AULA_SDP_SENDRECV &&
      remote_answer->audio.value.direction == AULA_SDP_SENDRECV;
}

static int aula_sdp_answer_directions_are_compatible(
    const aula_sdp_session *local_offer,
    const aula_sdp_session *remote_answer) {
  return aula_sdp_answer_direction_is_compatible(
             local_offer->video.value.direction,
             remote_answer->video.value.direction) &&
      aula_sdp_answer_direction_is_compatible(
          local_offer->audio.value.direction,
          remote_answer->audio.value.direction);
}

static void aula_sdp_log_direction_diagnostics(
    const aula_sdp_session *local_offer,
    const aula_sdp_session *remote_answer, int direct_crc_interop) {
  (void)fprintf(stderr,
                "aula-sipd: sdp-stage=answer-direction reason=%s "
                "video_offer=%d video_answer=%d audio_offer=%d audio_answer=%d\n",
                direct_crc_interop != 0 ? "direct-crc-sendonly" : "incompatible",
                (int)local_offer->video.value.direction,
                (int)remote_answer->video.value.direction,
                (int)local_offer->audio.value.direction,
                (int)remote_answer->audio.value.direction);
  (void)fflush(stderr);
}

static aula_status aula_sdp_validate_remote_answer_internal(
    const aula_sdp_session *local_offer, const aula_sdp_session *remote_answer,
    int allow_direct_crc_sendonly,
    aula_sdp_negotiated_session **out_negotiated) {
  aula_sdp_negotiated_session *negotiated;
  int direct_crc_interop;
  aula_status status;
  if (local_offer == NULL || remote_answer == NULL || out_negotiated == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = aula_sdp_validate_media(&local_offer->video, 1);
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_validate_media(&local_offer->audio, 0);
  }
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_validate_answer_media(&remote_answer->video, 1);
  }
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_validate_answer_media(&remote_answer->audio, 0);
  }
  if (status != AULA_STATUS_OK) return status;
  direct_crc_interop = allow_direct_crc_sendonly != 0 &&
      aula_sdp_direct_crc_sendonly_answer(local_offer, remote_answer);
  if (!aula_sdp_answer_directions_are_compatible(local_offer, remote_answer) &&
      !direct_crc_interop)
    return AULA_STATUS_UNSUPPORTED;
  negotiated = (aula_sdp_negotiated_session *)calloc(1U, sizeof(*negotiated));
  if (negotiated == NULL) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  status = aula_sdp_fill_negotiated_media(&negotiated->video,
                                            &negotiated->receive_video, local_offer,
                                            &local_offer->video, remote_answer,
                                            &remote_answer->video, &remote_answer->video, 1,
                                            &negotiated->video_srtp_lifetimes);
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_fill_negotiated_media(&negotiated->audio,
                                              &negotiated->receive_audio, local_offer,
                                              &local_offer->audio, remote_answer,
                                              &remote_answer->audio, &remote_answer->audio, 1,
                                              &negotiated->audio_srtp_lifetimes);
  }
  if (status != AULA_STATUS_OK) {
    aula_sdp_negotiated_session_destroy(negotiated);
    return status;
  }
  if (direct_crc_interop != 0) {
    negotiated->video.local_direction = AULA_SDP_SENDONLY;
    negotiated->audio.local_direction = AULA_SDP_SENDONLY;
  }
  *out_negotiated = negotiated;
  return AULA_STATUS_OK;
}

aula_status aula_sdp_validate_remote_answer(
    const aula_sdp_session *local_offer, const aula_sdp_session *remote_answer,
    aula_sdp_negotiated_session **out_negotiated) {
  return aula_sdp_validate_remote_answer_internal(
      local_offer, remote_answer, 0, out_negotiated);
}

static aula_status aula_sdp_negotiate_remote_answer_internal(
    const aula_sdp_session *local_offer, aula_bytes remote_answer,
    int allow_direct_crc_sendonly,
    aula_sdp_negotiated_session **out_negotiated) {
  aula_sdp_session *parsed_answer = NULL;
  int direct_crc_interop = 0;
  aula_status status;
  if (local_offer == NULL || out_negotiated == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = aula_sdp_parse_answer(remote_answer, &parsed_answer);
  if (status == AULA_STATUS_OK) {
    direct_crc_interop = allow_direct_crc_sendonly != 0 &&
        aula_sdp_direct_crc_sendonly_answer(local_offer, parsed_answer);
    status = aula_sdp_validate_remote_answer_internal(
        local_offer, parsed_answer, allow_direct_crc_sendonly, out_negotiated);
    if (status != AULA_STATUS_OK) {
      if (!aula_sdp_answer_directions_are_compatible(local_offer,
                                                       parsed_answer) &&
          !direct_crc_interop)
        aula_sdp_log_direction_diagnostics(local_offer, parsed_answer, 0);
      aula_sdp_log_codec_diagnostics(local_offer, 1, 0);
      aula_sdp_log_codec_diagnostics(parsed_answer, 0, 1);
    } else if (direct_crc_interop != 0) {
      aula_sdp_log_direction_diagnostics(local_offer, parsed_answer, 1);
    }
  }
  aula_sdp_session_destroy(parsed_answer);
  return status;
}

aula_status aula_sdp_negotiate_remote_answer(
    const aula_sdp_session *local_offer, aula_bytes remote_answer,
    aula_sdp_negotiated_session **out_negotiated) {
  return aula_sdp_negotiate_remote_answer_internal(
      local_offer, remote_answer, 0, out_negotiated);
}

aula_status aula_sdp_negotiate_remote_answer_direct_crc(
    const aula_sdp_session *local_offer, aula_bytes remote_answer,
    aula_sdp_negotiated_session **out_negotiated) {
  return aula_sdp_negotiate_remote_answer_internal(
      local_offer, remote_answer, 1, out_negotiated);
}

static aula_status aula_sdp_answer_remote_offer_with_srtp(
    const aula_sdp_session *remote_offer, const aula_sdp_answer_policy *policy,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_answer, aula_sdp_negotiated_session **out_negotiated) {
  aula_sdp_session *answer = NULL;
  aula_sdp_negotiated_session *negotiated;
  aula_status status;
  if (remote_offer == NULL || policy == NULL || out_answer == NULL ||
      out_negotiated == NULL || *out_answer != NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = aula_sdp_select_answer_with_srtp_policy(remote_offer, policy, video_srtp, audio_srtp,
                                                     &answer);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  negotiated = (aula_sdp_negotiated_session *)calloc(1U, sizeof(*negotiated));
  if (negotiated == NULL) {
    aula_sdp_session_destroy(answer);
    return AULA_STATUS_INTERNAL_ERROR;
  }
  status = aula_sdp_fill_negotiated_media(&negotiated->video,
                                            &negotiated->receive_video,
                                            answer, &answer->video,
                                            remote_offer, &remote_offer->video,
                                            &answer->video, 0,
                                            &negotiated->video_srtp_lifetimes);
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_fill_negotiated_media(&negotiated->audio,
                                              &negotiated->receive_audio,
                                              answer, &answer->audio,
                                              remote_offer, &remote_offer->audio,
                                              &answer->audio, 0,
                                              &negotiated->audio_srtp_lifetimes);
  }
  if (status != AULA_STATUS_OK) {
    aula_sdp_session_destroy(answer);
    aula_sdp_negotiated_session_destroy(negotiated);
    return status;
  }
  *out_answer = answer;
  *out_negotiated = negotiated;
  return AULA_STATUS_OK;
}

aula_status aula_sdp_answer_remote_offer(
    const aula_sdp_session *remote_offer, const aula_sdp_answer_policy *policy,
    aula_sdp_session **out_answer, aula_sdp_negotiated_session **out_negotiated) {
  return aula_sdp_answer_remote_offer_with_srtp(remote_offer, policy, NULL, NULL,
                                                 out_answer, out_negotiated);
}

aula_status aula_sdp_answer_remote_srtp_offer(
    const aula_sdp_session *remote_offer, const aula_sdp_answer_policy *policy,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_answer, aula_sdp_negotiated_session **out_negotiated) {
  if (video_srtp == NULL && audio_srtp == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  return aula_sdp_answer_remote_offer_with_srtp(remote_offer, policy, video_srtp, audio_srtp,
                                                 out_answer, out_negotiated);
}
