#include "sdp_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ls200_sdp_valid_assignment(const ls200_sdp_port_assignment *ports) {
  if (ports == NULL || ports->rtp_port == 0U || (ports->rtp_port & 1U) != 0U) {
    return 0;
  }
  if (ports->rtcp_mux != 0) {
    return ports->rtcp_port == ports->rtp_port;
  }
  return ports->rtp_port != 65535U && ports->rtcp_port != 0U &&
         ports->rtcp_port == (uint16_t)(ports->rtp_port + 1U);
}

static int ls200_sdp_codec_shape_is_supported(const ls200_sdp_codec *codec) {
  if (codec == NULL || codec->channels != 1U) {
    return 0;
  }
  switch (codec->kind) {
    case LS200_SDP_CODEC_H264:
      return codec->clock_rate == 90000U;
    case LS200_SDP_CODEC_PCMU:
    case LS200_SDP_CODEC_PCMA:
    case LS200_SDP_CODEC_TELEPHONE_EVENT:
      return codec->clock_rate == 8000U;
    default:
      return 0;
  }
}

static int ls200_sdp_capabilities_are_valid(const ls200_sdp_media_capabilities *capabilities) {
  return capabilities != NULL && ls200_sdp_valid_assignment(&capabilities->ports) &&
         capabilities->codecs != NULL && capabilities->codec_count != 0U &&
         capabilities->codec_count <= LS200_SDP_MAX_CODECS &&
         capabilities->direction >= LS200_SDP_SENDRECV &&
         capabilities->direction <= LS200_SDP_INACTIVE;
}

static int ls200_sdp_capability_codec_is_valid(const ls200_sdp_codec *codec, int video) {
  return codec->payload_type <= 127U && ls200_sdp_codec_shape_is_supported(codec) &&
         (!video || codec->kind == LS200_SDP_CODEC_H264) &&
         (video || codec->kind != LS200_SDP_CODEC_H264) &&
         (codec->format_parameters == NULL || codec->format_parameters[0] == '\0' ||
          (ls200_sdp_valid_text(codec->format_parameters) &&
           strlen(codec->format_parameters) < LS200_SDP_MAX_FMTP_BYTES));
}

static ls200_status ls200_sdp_copy_capability_codec(ls200_sdp_stored_media *target,
                                                     const ls200_sdp_codec *codec,
                                                     size_t index, int video) {
  ls200_status status;
  if (!ls200_sdp_capability_codec_is_valid(codec, video)) return LS200_STATUS_INVALID_ARGUMENT;
  status = ls200_sdp_add_codec(target, codec->kind, codec->payload_type,
                               codec->clock_rate, codec->channels);
  if (status != LS200_STATUS_OK) return status;
  target->declared_payload_types[codec->payload_type] = 1U;
  if (codec->format_parameters != NULL && codec->format_parameters[0] != '\0') {
    (void)memcpy(target->fmtp[index], codec->format_parameters,
                 strlen(codec->format_parameters) + 1U);
  }
  return LS200_STATUS_OK;
}

static void ls200_sdp_copy_capabilities_transport(
    ls200_sdp_stored_media *target, const ls200_sdp_media_capabilities *capabilities,
    int video) {
  target->present = 1;
  target->is_video = video;
  target->value.rtp_port = capabilities->ports.rtp_port;
  target->value.rtcp_port = capabilities->ports.rtcp_port;
  target->value.rtcp_mux = capabilities->ports.rtcp_mux != 0;
  target->value.direction = capabilities->direction;
  target->value.codecs = target->codecs;
}

static ls200_status ls200_sdp_copy_capabilities_media(
    ls200_sdp_stored_media *target, const ls200_sdp_media_capabilities *capabilities,
    int video) {
  size_t index;
  if (target == NULL || !ls200_sdp_capabilities_are_valid(capabilities))
    return LS200_STATUS_INVALID_ARGUMENT;
  ls200_sdp_copy_capabilities_transport(target, capabilities, video);
  for (index = 0U; index < capabilities->codec_count; ++index) {
    ls200_status status = ls200_sdp_copy_capability_codec(target,
        &capabilities->codecs[index], index, video);
    if (status != LS200_STATUS_OK) return status;
  }
  return ls200_sdp_validate_media(target, video);
}

ls200_status ls200_sdp_create_initial_offer(
    const ls200_sdp_local_capabilities *capabilities, ls200_sdp_session **out_offer) {
  ls200_sdp_session *offer;
  ls200_status status;
  if (capabilities == NULL || out_offer == NULL || *out_offer != NULL ||
      capabilities->local_address == NULL ||
      strlen(capabilities->local_address) >= LS200_SDP_MAX_ADDRESS_BYTES ||
      ls200_sdp_validate_local_ipv4(capabilities->local_address) != LS200_STATUS_OK ||
      capabilities->remote_target_authorizer == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  offer = (ls200_sdp_session *)calloc(1U, sizeof(*offer));
  if (offer == NULL) {
    return LS200_STATUS_INTERNAL_ERROR;
  }
  (void)memcpy(offer->connection_address, capabilities->local_address,
               strlen(capabilities->local_address) + 1U);
  offer->remote_target_authorizer = capabilities->remote_target_authorizer;
  offer->remote_target_context = capabilities->remote_target_context;
  status = ls200_sdp_copy_capabilities_media(&offer->video, &capabilities->video, 1);
  if (status == LS200_STATUS_OK) {
    status = ls200_sdp_copy_capabilities_media(&offer->audio, &capabilities->audio, 0);
  }
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(offer);
    return status;
  }
  *out_offer = offer;
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_create_initial_srtp_offer(
    const ls200_sdp_local_capabilities *capabilities,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_offer) {
  ls200_status status;
  if ((video_srtp == NULL && audio_srtp == NULL) || out_offer == NULL || *out_offer != NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_sdp_create_initial_offer(capabilities, out_offer);
  if (status != LS200_STATUS_OK) return status;
  if (video_srtp != NULL) status = ls200_sdp_assign_srtp_policy(&(*out_offer)->video, video_srtp);
  if (status == LS200_STATUS_OK && audio_srtp != NULL) {
    status = ls200_sdp_assign_srtp_policy(&(*out_offer)->audio, audio_srtp);
  }
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(*out_offer);
    *out_offer = NULL;
  }
  return status;
}

static ls200_sdp_direction ls200_sdp_answer_direction(ls200_sdp_direction offer) {
  if (offer == LS200_SDP_SENDONLY) {
    return LS200_SDP_RECVONLY;
  }
  if (offer == LS200_SDP_RECVONLY) {
    return LS200_SDP_SENDONLY;
  }
  return offer;
}

int ls200_sdp_answer_direction_is_compatible(ls200_sdp_direction offer,
                                             ls200_sdp_direction answer) {
  if (offer == LS200_SDP_SENDRECV) {
    return answer >= LS200_SDP_SENDRECV && answer <= LS200_SDP_INACTIVE;
  }
  if (offer == LS200_SDP_SENDONLY) {
    return answer == LS200_SDP_RECVONLY || answer == LS200_SDP_INACTIVE;
  }
  if (offer == LS200_SDP_RECVONLY) {
    return answer == LS200_SDP_SENDONLY || answer == LS200_SDP_INACTIVE;
  }
  return offer == LS200_SDP_INACTIVE && answer == LS200_SDP_INACTIVE;
}

static ls200_status ls200_sdp_copy_selected_codec(ls200_sdp_stored_media *target,
                                                  const ls200_sdp_stored_media *source,
                                                  ls200_sdp_codec_kind kind) {
  size_t index;
  for (index = 0U; index < source->value.codec_count; ++index) {
    if (source->codecs[index].kind == kind) {
      ls200_status status = ls200_sdp_add_codec(target, kind, source->codecs[index].payload_type,
                                                source->codecs[index].clock_rate,
                                                source->codecs[index].channels);
      if (status == LS200_STATUS_OK && source->codecs[index].format_parameters[0] != '\0') {
        size_t target_index = target->value.codec_count - 1U;
        (void)memcpy(target->fmtp[target_index], source->codecs[index].format_parameters,
                     strlen(source->codecs[index].format_parameters) + 1U);
      }
      return status;
    }
  }
  return LS200_STATUS_UNSUPPORTED;
}

static void ls200_sdp_copy_transport(ls200_sdp_stored_media *target,
                                     const ls200_sdp_stored_media *source) {
  target->present = 1;
  target->is_video = source->is_video;
  target->value.rtp_port = source->value.rtp_port;
  target->value.rtcp_port = source->value.rtcp_port;
  target->value.rtcp_mux = source->value.rtcp_mux;
  target->value.profile = source->value.profile;
  target->value.direction = ls200_sdp_answer_direction(source->value.direction);
  target->value.codecs = target->codecs;
  target->value.codec_count = 0U;
}

ls200_status ls200_sdp_select_answer(const ls200_sdp_session *offer,
                                     ls200_sdp_session **out_answer) {
  ls200_sdp_session *answer;
  ls200_status status;
  if (offer == NULL || out_answer == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_answer = NULL;
  status = ls200_sdp_validate_media(&offer->video, 1);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  status = ls200_sdp_validate_media(&offer->audio, 0);
  if (status != LS200_STATUS_OK) {
    return status;
  }
  if (offer->video.value.profile == LS200_SDP_MEDIA_PROFILE_RTP_SAVP ||
      offer->audio.value.profile == LS200_SDP_MEDIA_PROFILE_RTP_SAVP) {
    return LS200_STATUS_UNSUPPORTED;
  }
  answer = (ls200_sdp_session *)calloc(1U, sizeof(*answer));
  if (answer == NULL) {
    return LS200_STATUS_INTERNAL_ERROR;
  }
  (void)memcpy(answer->connection_address, offer->connection_address,
               sizeof(answer->connection_address));
  answer->remote_target_authorizer = offer->remote_target_authorizer;
  answer->remote_target_context = offer->remote_target_context;
  ls200_sdp_copy_transport(&answer->video, &offer->video);
  ls200_sdp_copy_transport(&answer->audio, &offer->audio);
  status = ls200_sdp_copy_selected_codec(&answer->video, &offer->video, LS200_SDP_CODEC_H264);
  if (status == LS200_STATUS_OK) {
    if (ls200_sdp_media_has_kind(&offer->audio, LS200_SDP_CODEC_PCMU)) {
      status = ls200_sdp_copy_selected_codec(&answer->audio, &offer->audio, LS200_SDP_CODEC_PCMU);
    } else {
      status = ls200_sdp_copy_selected_codec(&answer->audio, &offer->audio, LS200_SDP_CODEC_PCMA);
    }
  }
  if (status == LS200_STATUS_OK &&
      ls200_sdp_media_has_kind(&offer->audio,
                               LS200_SDP_CODEC_TELEPHONE_EVENT)) {
    status = ls200_sdp_copy_selected_codec(&answer->audio, &offer->audio,
                                           LS200_SDP_CODEC_TELEPHONE_EVENT);
  }
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(answer);
    return status;
  }
  *out_answer = answer;
  return LS200_STATUS_OK;
}

static ls200_status ls200_sdp_copy_policy_codec(ls200_sdp_stored_media *target,
                                                 const ls200_sdp_stored_media *offer,
                                                 ls200_sdp_codec_kind kind,
                                                 int allowed) {
  if (!allowed) return LS200_STATUS_UNSUPPORTED;
  return ls200_sdp_copy_selected_codec(target, offer, kind);
}

static int ls200_sdp_policy_is_valid(const ls200_sdp_answer_policy *policy) {
  return policy != NULL && ls200_sdp_valid_assignment(&policy->video_ports) &&
         ls200_sdp_valid_assignment(&policy->audio_ports) &&
         policy->video_direction >= LS200_SDP_SENDRECV &&
         policy->video_direction <= LS200_SDP_INACTIVE &&
         policy->audio_direction >= LS200_SDP_SENDRECV &&
         policy->audio_direction <= LS200_SDP_INACTIVE &&
         policy->local_address != NULL &&
         strlen(policy->local_address) < LS200_SDP_MAX_ADDRESS_BYTES &&
         ls200_sdp_validate_local_ipv4(policy->local_address) == LS200_STATUS_OK &&
         policy->remote_target_authorizer != NULL;
}

static void ls200_sdp_apply_policy_media(ls200_sdp_stored_media *media,
                                         const ls200_sdp_port_assignment *ports,
                                         ls200_sdp_direction direction) {
  media->value.rtp_port = ports->rtp_port;
  media->value.rtcp_port = ports->rtcp_port;
  media->value.rtcp_mux = ports->rtcp_mux != 0;
  media->value.direction = direction;
}

static ls200_status ls200_sdp_apply_policy_srtp(
    ls200_sdp_stored_media *answer, const ls200_sdp_stored_media *offer,
    const ls200_sdp_srtp_policy *policy) {
  if (answer == NULL || offer == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (offer->value.profile == LS200_SDP_MEDIA_PROFILE_RTP_AVP) return LS200_STATUS_OK;
  if (offer->value.profile != LS200_SDP_MEDIA_PROFILE_RTP_SAVP) return LS200_STATUS_INVALID_DATA;
  if (policy == NULL) return LS200_STATUS_UNSUPPORTED;
  {
    ls200_status status = ls200_sdp_assign_srtp_policy(answer, policy);
    if (status == LS200_STATUS_OK) answer->srtp.crypto_tag = offer->srtp.crypto_tag;
    return status;
  }
}

static ls200_status ls200_sdp_select_policy_audio(ls200_sdp_stored_media *target,
                                                   const ls200_sdp_stored_media *offer,
                                                   const ls200_sdp_answer_policy *policy) {
  if (policy->allow_pcmu && ls200_sdp_media_has_kind(offer, LS200_SDP_CODEC_PCMU))
    return ls200_sdp_copy_selected_codec(target, offer, LS200_SDP_CODEC_PCMU);
  if (policy->allow_pcma && ls200_sdp_media_has_kind(offer, LS200_SDP_CODEC_PCMA))
    return ls200_sdp_copy_selected_codec(target, offer, LS200_SDP_CODEC_PCMA);
  return LS200_STATUS_UNSUPPORTED;
}

static ls200_status ls200_sdp_select_policy_codecs(ls200_sdp_session *answer,
                                                    const ls200_sdp_session *offer,
                                                    const ls200_sdp_answer_policy *policy) {
  ls200_status status = ls200_sdp_copy_policy_codec(&answer->video, &offer->video,
                                                     LS200_SDP_CODEC_H264, 1);
  if (status == LS200_STATUS_OK) status = ls200_sdp_select_policy_audio(&answer->audio,
      &offer->audio, policy);
  if (status == LS200_STATUS_OK && policy->allow_telephone_event &&
      ls200_sdp_media_has_kind(&offer->audio, LS200_SDP_CODEC_TELEPHONE_EVENT)) {
    status = ls200_sdp_copy_policy_codec(&answer->audio, &offer->audio,
        LS200_SDP_CODEC_TELEPHONE_EVENT, policy->allow_telephone_event);
  }
  return status;
}

static ls200_status ls200_sdp_validate_policy_offer(
    const ls200_sdp_session *offer, const ls200_sdp_answer_policy *policy) {
  ls200_status status;
  status = ls200_sdp_validate_media(&offer->video, 1);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_sdp_validate_media(&offer->audio, 0);
  if (status != LS200_STATUS_OK) return status;
  if (!policy->allow_h264_packetization_mode_1) return LS200_STATUS_UNSUPPORTED;
  if (!ls200_sdp_answer_direction_is_compatible(offer->video.value.direction,
                                                policy->video_direction) ||
      !ls200_sdp_answer_direction_is_compatible(offer->audio.value.direction,
                                                policy->audio_direction))
    return LS200_STATUS_UNSUPPORTED;
  return LS200_STATUS_OK;
}

static ls200_sdp_session *ls200_sdp_create_policy_answer(
    const ls200_sdp_session *offer, const ls200_sdp_answer_policy *policy) {
  ls200_sdp_session *answer = (ls200_sdp_session *)calloc(1U, sizeof(*answer));
  if (answer == NULL) return NULL;
  (void)memcpy(answer->connection_address, policy->local_address,
               strlen(policy->local_address) + 1U);
  answer->remote_target_authorizer = policy->remote_target_authorizer;
  answer->remote_target_context = policy->remote_target_context;
  ls200_sdp_copy_transport(&answer->video, &offer->video);
  ls200_sdp_copy_transport(&answer->audio, &offer->audio);
  ls200_sdp_apply_policy_media(&answer->video, &policy->video_ports, policy->video_direction);
  ls200_sdp_apply_policy_media(&answer->audio, &policy->audio_ports, policy->audio_direction);
  return answer;
}

static ls200_status ls200_sdp_complete_policy_answer(
    ls200_sdp_session *answer, const ls200_sdp_session *offer,
    const ls200_sdp_answer_policy *policy, const ls200_sdp_srtp_policy *video_srtp,
    const ls200_sdp_srtp_policy *audio_srtp) {
  ls200_status status = ls200_sdp_apply_policy_srtp(&answer->video, &offer->video,
                                                     video_srtp);
  if (status == LS200_STATUS_OK)
    status = ls200_sdp_apply_policy_srtp(&answer->audio, &offer->audio, audio_srtp);
  if (status == LS200_STATUS_OK)
    status = ls200_sdp_select_policy_codecs(answer, offer, policy);
  if (status == LS200_STATUS_OK) status = ls200_sdp_validate_media(&answer->video, 1);
  if (status == LS200_STATUS_OK) status = ls200_sdp_validate_media(&answer->audio, 0);
  return status;
}

ls200_status ls200_sdp_select_answer_with_srtp_policy(
    const ls200_sdp_session *offer, const ls200_sdp_answer_policy *policy,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_answer) {
  ls200_sdp_session *answer;
  ls200_status status;
  if (offer == NULL || out_answer == NULL || *out_answer != NULL ||
      !ls200_sdp_policy_is_valid(policy)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_sdp_validate_policy_offer(offer, policy);
  if (status != LS200_STATUS_OK) return status;
  answer = ls200_sdp_create_policy_answer(offer, policy);
  if (answer == NULL) return LS200_STATUS_INTERNAL_ERROR;
  status = ls200_sdp_complete_policy_answer(answer, offer, policy, video_srtp, audio_srtp);
  if (status != LS200_STATUS_OK) {
    ls200_sdp_session_destroy(answer);
    return status;
  }
  *out_answer = answer;
  return LS200_STATUS_OK;
}

ls200_status ls200_sdp_select_answer_with_policy(const ls200_sdp_session *offer,
                                                 const ls200_sdp_answer_policy *policy,
                                                 ls200_sdp_session **out_answer) {
  return ls200_sdp_select_answer_with_srtp_policy(offer, policy, NULL, NULL, out_answer);
}
