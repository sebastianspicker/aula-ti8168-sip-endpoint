#include "sdp_internal.h"
#include "../media/h264_profile_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls200_status ls200_sdp_copy_h264_evidence(ls200_sdp_h264_evidence *evidence,
                                                  const char *fmtp) {
  char copy[LS200_SDP_MAX_FMTP_BYTES];
  char *field;
  if (evidence == NULL || !ls200_sdp_h264_fmtp_is_compatible(fmtp) ||
      strlen(fmtp) >= sizeof(copy)) {
    return LS200_STATUS_INVALID_DATA;
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
      return LS200_STATUS_INVALID_DATA;
    }
    *equals++ = '\0';
    if (ls200_sdp_ascii_equal(field, "profile-level-id")) {
      (void)memcpy(evidence->profile_level_id, equals, 6U);
      evidence->profile_level_id[6] = '\0';
    } else if (ls200_sdp_ascii_equal(field, "sprop-parameter-sets")) {
      if (*equals == '\0' || strlen(equals) >= sizeof(evidence->parameter_sets)) {
        return LS200_STATUS_INVALID_DATA;
      }
      (void)memcpy(evidence->parameter_sets, equals, strlen(equals) + 1U);
      evidence->has_parameter_sets = 1;
    }
    field = strtok(NULL, ";");
  }
  return evidence->profile_level_id[0] == '\0' ? LS200_STATUS_INVALID_DATA : LS200_STATUS_OK;
}

static int ls200_sdp_codec_matches(const ls200_sdp_codec *left,
                                   const ls200_sdp_codec *right) {
  ls200_sdp_h264_evidence left_h264;
  ls200_sdp_h264_evidence right_h264;
  if (left == NULL || right == NULL || left->kind != right->kind ||
      left->clock_rate != right->clock_rate || left->channels != right->channels ||
      left->format_parameters == NULL ||
      right->format_parameters == NULL) {
    return 0;
  }
  if (left->kind == LS200_SDP_CODEC_H264) {
    (void)memset(&left_h264, 0, sizeof(left_h264));
    (void)memset(&right_h264, 0, sizeof(right_h264));
    if (ls200_sdp_copy_h264_evidence(&left_h264, left->format_parameters) !=
            LS200_STATUS_OK ||
        ls200_sdp_copy_h264_evidence(&right_h264, right->format_parameters) !=
            LS200_STATUS_OK) {
      return 0;
    }
    /* Each sender advertises the SPS/PPS for its own encoded stream.  Those
     * values need not be byte-identical in an answer; the interoperable
     * contract is the selected payload, packetization mode, and profile. */
    return left_h264.packetization_mode == right_h264.packetization_mode &&
           ls200_h264_profile_level_id_compatible(
               left_h264.profile_level_id, right_h264.profile_level_id);
  }
  return strcmp(left->format_parameters, right->format_parameters) == 0;
}

static const ls200_sdp_codec *ls200_sdp_find_matching_codec(
    const ls200_sdp_stored_media *media, const ls200_sdp_codec *selected) {
  size_t index;
  for (index = 0U; media != NULL && index < media->value.codec_count; ++index) {
    if (ls200_sdp_codec_matches(&media->codecs[index], selected))
      return &media->codecs[index];
  }
  return NULL;
}

static ls200_status ls200_sdp_authorize_negotiated_targets(
    const ls200_sdp_session *local, const ls200_sdp_session *remote,
    const ls200_sdp_stored_media *remote_media, const char **out_rtcp_address) {
  const char *rtcp_address = remote_media->rtcp_address[0] == '\0' ?
                                 remote->connection_address : remote_media->rtcp_address;
  if (ls200_sdp_authorize_remote_ipv4(local, remote->connection_address,
                                      remote_media->value.rtp_port, 0) != LS200_STATUS_OK ||
      ls200_sdp_authorize_remote_ipv4(local, rtcp_address,
                                      remote_media->value.rtcp_port, 1) != LS200_STATUS_OK)
    return LS200_STATUS_SECURITY_ERROR;
  *out_rtcp_address = rtcp_address;
  return LS200_STATUS_OK;
}

static ls200_sdp_direction ls200_sdp_effective_local_answer_direction(
    ls200_sdp_direction remote_direction) {
  switch (remote_direction) {
    case LS200_SDP_SENDRECV: return LS200_SDP_SENDRECV;
    case LS200_SDP_SENDONLY: return LS200_SDP_RECVONLY;
    case LS200_SDP_RECVONLY: return LS200_SDP_SENDONLY;
    case LS200_SDP_INACTIVE: return LS200_SDP_INACTIVE;
  }
  return LS200_SDP_INACTIVE;
}

static void ls200_sdp_copy_negotiated_transport(
    ls200_sdp_negotiated_media *target, const ls200_sdp_stored_media *local_media,
    const ls200_sdp_session *remote, const ls200_sdp_stored_media *remote_media,
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
      ls200_sdp_effective_local_answer_direction(remote_media->value.direction) :
      local_media->value.direction;
  target->remote_direction = remote_media->value.direction;
}

static ls200_status ls200_sdp_copy_negotiated_srtp(
    ls200_sdp_negotiated_media *target, const ls200_sdp_stored_media *local_media,
    const ls200_sdp_stored_media *remote_media,
    ls200_sdp_srtp_lifetimes *lifetimes) {
  if (target == NULL || local_media == NULL || remote_media == NULL ||
      lifetimes == NULL || local_media->value.profile != remote_media->value.profile) {
    return LS200_STATUS_UNSUPPORTED;
  }
  if (local_media->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_AVP) {
    return !local_media->srtp.present && !remote_media->srtp.present ? LS200_STATUS_OK :
                                                                        LS200_STATUS_INVALID_DATA;
  }
  if (local_media->value.profile != LS200_SDP_MEDIA_PROFILE_RTP_SAVP ||
      !local_media->srtp.present || !remote_media->srtp.present ||
      local_media->srtp.crypto_tag != remote_media->srtp.crypto_tag) {
    return LS200_STATUS_INVALID_DATA;
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
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_copy_codec_fields(
    ls200_sdp_negotiated_codec *codec, const ls200_sdp_codec *selected) {
  if (selected->format_parameters == NULL ||
      strlen(selected->format_parameters) >= sizeof(codec->format_parameters))
    return LS200_STATUS_INVALID_DATA;
  codec->kind = selected->kind;
  codec->payload_type = selected->payload_type;
  codec->clock_rate = selected->clock_rate;
  codec->channels = selected->channels;
  (void)memcpy(codec->format_parameters, selected->format_parameters,
               strlen(selected->format_parameters) + 1U);
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_copy_negotiated_codec(
    ls200_sdp_negotiated_media *target, size_t index,
    const ls200_sdp_codec *selected) {
  ls200_status status = ls200_sdp_copy_codec_fields(&target->codecs[index], selected);
  if (status != LS200_STATUS_OK) return status;
  return selected->kind == LS200_SDP_CODEC_H264 ?
             ls200_sdp_copy_h264_evidence(&target->h264, selected->format_parameters) :
             LS200_STATUS_OK;
}

static ls200_status ls200_sdp_copy_receive_codec(
    ls200_sdp_receive_media *target, size_t index,
    const ls200_sdp_codec *selected) {
  return ls200_sdp_copy_codec_fields(&target->codecs[index], selected);
}

static ls200_status ls200_sdp_copy_negotiated_codecs(
    ls200_sdp_negotiated_media *target, ls200_sdp_receive_media *receive,
    const ls200_sdp_stored_media *receive_media,
    const ls200_sdp_stored_media *selected_media) {
  size_t index;
  size_t selected_count = 0U;
  for (index = 0U; index < selected_media->value.codec_count; ++index) {
    const ls200_sdp_codec *selected = &selected_media->codecs[index];
    const ls200_sdp_codec *offered = ls200_sdp_find_matching_codec(
        receive_media, selected);
    ls200_status status;
    if (offered == NULL) continue;
    if (selected_count >= LS200_SDP_MAX_SELECTED_CODECS)
      return LS200_STATUS_LIMIT_EXCEEDED;
    status = ls200_sdp_copy_negotiated_codec(target, selected_count, selected);
    if (status == LS200_STATUS_OK)
      status = ls200_sdp_copy_receive_codec(receive, selected_count, offered);
    if (status != LS200_STATUS_OK) return status;
    ++selected_count;
  }
  target->codec_count = selected_count;
  receive->codec_count = selected_count;
  return selected_count != 0U ? LS200_STATUS_OK : LS200_STATUS_UNSUPPORTED;
}

static void ls200_sdp_copy_negotiated_feedback(
    const ls200_sdp_negotiated_media *target, ls200_sdp_receive_media *receive,
    const ls200_sdp_stored_media *local, const ls200_sdp_stored_media *remote,
    int remote_is_answer) {
  size_t index;
  for (index = 0U; index < target->codec_count; ++index) {
    uint8_t selected = target->codecs[index].payload_type;
    uint8_t offered = receive->codecs[index].payload_type;
    if (target->codecs[index].kind != LS200_SDP_CODEC_H264) continue;
    receive->feedback_mask = ls200_sdp_media_feedback(local,
        remote_is_answer != 0 ? offered : selected) &
        ls200_sdp_media_feedback(remote, remote_is_answer != 0 ? selected : offered);
    return;
  }
}

static ls200_status ls200_sdp_fill_negotiated_media(
    ls200_sdp_negotiated_media *target, ls200_sdp_receive_media *receive,
    const ls200_sdp_session *local,
    const ls200_sdp_stored_media *local_media, const ls200_sdp_session *remote,
    const ls200_sdp_stored_media *remote_media, const ls200_sdp_stored_media *selected_media,
    int remote_is_answer,
    ls200_sdp_srtp_lifetimes *lifetimes) {
  const char *rtcp_address;
  ls200_status status;
  if (target == NULL || local_media == NULL || remote == NULL || remote_media == NULL ||
      selected_media == NULL ||
      selected_media->value.codec_count == 0U ||
      selected_media->value.codec_count > LS200_SDP_MAX_SELECTED_CODECS) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (ls200_sdp_authorize_negotiated_targets(local, remote, remote_media,
                                             &rtcp_address) != LS200_STATUS_OK)
    return LS200_STATUS_SECURITY_ERROR;
  ls200_sdp_copy_negotiated_transport(target, local_media, remote, remote_media,
                                      rtcp_address, remote_is_answer);
  if (ls200_sdp_copy_negotiated_srtp(target, local_media, remote_media,
                                     lifetimes) != LS200_STATUS_OK) {
    return LS200_STATUS_UNSUPPORTED;
  }
  status = ls200_sdp_copy_negotiated_codecs(
      target, receive, remote_is_answer != 0 ? local_media : remote_media,
      selected_media);
  if (status == LS200_STATUS_OK)
    ls200_sdp_copy_negotiated_feedback(target, receive, local_media, remote_media,
                                       remote_is_answer);
  return status;
}

static int ls200_sdp_direct_crc_sendonly_answer(
    const ls200_sdp_session *local_offer,
    const ls200_sdp_session *remote_answer) {
  return local_offer->video.value.direction == LS200_SDP_SENDONLY &&
      local_offer->audio.value.direction == LS200_SDP_SENDONLY &&
      remote_answer->video.value.direction == LS200_SDP_SENDRECV &&
      remote_answer->audio.value.direction == LS200_SDP_SENDRECV;
}

static int ls200_sdp_answer_directions_are_compatible(
    const ls200_sdp_session *local_offer,
    const ls200_sdp_session *remote_answer) {
  return ls200_sdp_answer_direction_is_compatible(
             local_offer->video.value.direction,
             remote_answer->video.value.direction) &&
      ls200_sdp_answer_direction_is_compatible(
          local_offer->audio.value.direction,
          remote_answer->audio.value.direction);
}

static void ls200_sdp_log_direction_diagnostics(
    const ls200_sdp_session *local_offer,
    const ls200_sdp_session *remote_answer, int direct_crc_interop) {
  (void)fprintf(stderr,
                "ls200-sipd: sdp-stage=answer-direction reason=%s "
                "video_offer=%d video_answer=%d audio_offer=%d audio_answer=%d\n",
                direct_crc_interop != 0 ? "direct-crc-sendonly" : "incompatible",
                (int)local_offer->video.value.direction,
                (int)remote_answer->video.value.direction,
                (int)local_offer->audio.value.direction,
                (int)remote_answer->audio.value.direction);
  (void)fflush(stderr);
}

static ls200_status ls200_sdp_validate_remote_answer_internal(
    const ls200_sdp_session *local_offer, const ls200_sdp_session *remote_answer,
    int allow_direct_crc_sendonly,
    ls200_sdp_negotiated_session **out_negotiated) {
  ls200_sdp_negotiated_session *negotiated;
  int direct_crc_interop;
  ls200_status status;
  if (local_offer == NULL || remote_answer == NULL || out_negotiated == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = ls200_sdp_validate_media(&local_offer->video, 1);
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_validate_media(&local_offer->audio, 0);
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_validate_answer_media(&remote_answer->video, 1);
  }
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_validate_answer_media(&remote_answer->audio, 0);
  }
  if (status != LS200_STATUS_OK) return status;
  direct_crc_interop = allow_direct_crc_sendonly != 0 &&
      ls200_sdp_direct_crc_sendonly_answer(local_offer, remote_answer);
  if (!ls200_sdp_answer_directions_are_compatible(local_offer, remote_answer) &&
      !direct_crc_interop)
    return LS200_STATUS_UNSUPPORTED;
  negotiated = (ls200_sdp_negotiated_session *)calloc(1U, sizeof(*negotiated));
  if (negotiated == NULL) {
    return LS200_STATUS_INTERNAL_ERROR;
  }
  status = ls200_sdp_fill_negotiated_media(&negotiated->video,
                                            &negotiated->receive_video, local_offer,
                                            &local_offer->video, remote_answer,
                                            &remote_answer->video, &remote_answer->video, 1,
                                            &negotiated->video_srtp_lifetimes);
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_fill_negotiated_media(&negotiated->audio,
                                              &negotiated->receive_audio, local_offer,
                                              &local_offer->audio, remote_answer,
                                              &remote_answer->audio, &remote_answer->audio, 1,
                                              &negotiated->audio_srtp_lifetimes);
  }
  if (status != LS200_STATUS_OK) {
    ls200_sdp_negotiated_session_destroy(negotiated);
    return status;
  }
  if (direct_crc_interop != 0) {
    negotiated->video.local_direction = LS200_SDP_SENDONLY;
    negotiated->audio.local_direction = LS200_SDP_SENDONLY;
  }
  *out_negotiated = negotiated;
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_validate_remote_answer(
    const ls200_sdp_session *local_offer, const ls200_sdp_session *remote_answer,
    ls200_sdp_negotiated_session **out_negotiated) {
  return ls200_sdp_validate_remote_answer_internal(
      local_offer, remote_answer, 0, out_negotiated);
}

static ls200_status ls200_sdp_negotiate_remote_answer_internal(
    const ls200_sdp_session *local_offer, ls200_bytes remote_answer,
    int allow_direct_crc_sendonly,
    ls200_sdp_negotiated_session **out_negotiated) {
  ls200_sdp_session *parsed_answer = NULL;
  int direct_crc_interop = 0;
  ls200_status status;
  if (local_offer == NULL || out_negotiated == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = ls200_sdp_parse_answer(remote_answer, &parsed_answer);
  if (status == LS200_STATUS_OK) {
    direct_crc_interop = allow_direct_crc_sendonly != 0 &&
        ls200_sdp_direct_crc_sendonly_answer(local_offer, parsed_answer);
    status = ls200_sdp_validate_remote_answer_internal(
        local_offer, parsed_answer, allow_direct_crc_sendonly, out_negotiated);
    if (status != LS200_STATUS_OK) {
      if (!ls200_sdp_answer_directions_are_compatible(local_offer,
                                                       parsed_answer) &&
          !direct_crc_interop)
        ls200_sdp_log_direction_diagnostics(local_offer, parsed_answer, 0);
      ls200_sdp_log_codec_diagnostics(local_offer, 1, 0);
      ls200_sdp_log_codec_diagnostics(parsed_answer, 0, 1);
    } else if (direct_crc_interop != 0) {
      ls200_sdp_log_direction_diagnostics(local_offer, parsed_answer, 1);
    }
  }
  ls200_sdp_session_destroy(parsed_answer);
  return status;
}

ls200_status ls200_sdp_negotiate_remote_answer(
    const ls200_sdp_session *local_offer, ls200_bytes remote_answer,
    ls200_sdp_negotiated_session **out_negotiated) {
  return ls200_sdp_negotiate_remote_answer_internal(
      local_offer, remote_answer, 0, out_negotiated);
}

ls200_status ls200_sdp_negotiate_remote_answer_direct_crc(
    const ls200_sdp_session *local_offer, ls200_bytes remote_answer,
    ls200_sdp_negotiated_session **out_negotiated) {
  return ls200_sdp_negotiate_remote_answer_internal(
      local_offer, remote_answer, 1, out_negotiated);
}

static ls200_status ls200_sdp_answer_remote_offer_with_srtp(
    const ls200_sdp_session *remote_offer, const ls200_sdp_answer_policy *policy,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_answer, ls200_sdp_negotiated_session **out_negotiated) {
  ls200_sdp_session *answer = NULL;
  ls200_sdp_negotiated_session *negotiated;
  ls200_status status;
  if (remote_offer == NULL || policy == NULL || out_answer == NULL ||
      out_negotiated == NULL || *out_answer != NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_negotiated = NULL;
  status = ls200_sdp_select_answer_with_srtp_policy(remote_offer, policy, video_srtp, audio_srtp,
                                                     &answer);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  negotiated = (ls200_sdp_negotiated_session *)calloc(1U, sizeof(*negotiated));
  if (negotiated == NULL) {
    ls200_sdp_session_destroy(answer);
    return LS200_STATUS_INTERNAL_ERROR;
  }
  status = ls200_sdp_fill_negotiated_media(&negotiated->video,
                                            &negotiated->receive_video,
                                            answer, &answer->video,
                                            remote_offer, &remote_offer->video,
                                            &answer->video, 0,
                                            &negotiated->video_srtp_lifetimes);
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_fill_negotiated_media(&negotiated->audio,
                                              &negotiated->receive_audio,
                                              answer, &answer->audio,
                                              remote_offer, &remote_offer->audio,
                                              &answer->audio, 0,
                                              &negotiated->audio_srtp_lifetimes);
  }
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(answer);
    ls200_sdp_negotiated_session_destroy(negotiated);
    return status;
  }
  *out_answer = answer;
  *out_negotiated = negotiated;
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_answer_remote_offer(
    const ls200_sdp_session *remote_offer, const ls200_sdp_answer_policy *policy,
    ls200_sdp_session **out_answer, ls200_sdp_negotiated_session **out_negotiated) {
  return ls200_sdp_answer_remote_offer_with_srtp(remote_offer, policy, NULL, NULL,
                                                 out_answer, out_negotiated);
}

ls200_status ls200_sdp_answer_remote_srtp_offer(
    const ls200_sdp_session *remote_offer, const ls200_sdp_answer_policy *policy,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_answer, ls200_sdp_negotiated_session **out_negotiated) {
  if (video_srtp == NULL && audio_srtp == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  return ls200_sdp_answer_remote_offer_with_srtp(remote_offer, policy, video_srtp, audio_srtp,
                                                 out_answer, out_negotiated);
}
