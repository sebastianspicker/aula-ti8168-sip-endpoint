#include "sdp_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int aula_sdp_valid_assignment(const aula_sdp_port_assignment *ports) {
  if (ports == NULL || ports->rtp_port == 0U || (ports->rtp_port & 1U) != 0U) {
    return 0;
  }
  if (ports->rtcp_mux != 0) {
    return ports->rtcp_port == ports->rtp_port;
  }
  return ports->rtp_port != 65535U && ports->rtcp_port != 0U &&
         ports->rtcp_port == (uint16_t)(ports->rtp_port + 1U);
}

static int aula_sdp_codec_shape_is_supported(const aula_sdp_codec *codec) {
  if (codec == NULL || codec->channels != 1U) {
    return 0;
  }
  switch (codec->kind) {
    case AULA_SDP_CODEC_H264:
      return codec->clock_rate == 90000U;
    case AULA_SDP_CODEC_PCMU:
    case AULA_SDP_CODEC_PCMA:
    case AULA_SDP_CODEC_TELEPHONE_EVENT:
      return codec->clock_rate == 8000U;
    default:
      return 0;
  }
}

static int aula_sdp_capabilities_are_valid(const aula_sdp_media_capabilities *capabilities) {
  return capabilities != NULL && aula_sdp_valid_assignment(&capabilities->ports) &&
         capabilities->codecs != NULL && capabilities->codec_count != 0U &&
         capabilities->codec_count <= AULA_SDP_MAX_CODECS &&
         capabilities->direction >= AULA_SDP_SENDRECV &&
         capabilities->direction <= AULA_SDP_INACTIVE;
}

static int aula_sdp_capability_codec_is_valid(const aula_sdp_codec *codec, int video) {
  return codec->payload_type <= 127U && aula_sdp_codec_shape_is_supported(codec) &&
         (!video || codec->kind == AULA_SDP_CODEC_H264) &&
         (video || codec->kind != AULA_SDP_CODEC_H264) &&
         (codec->format_parameters == NULL || codec->format_parameters[0] == '\0' ||
          (aula_sdp_valid_text(codec->format_parameters) &&
           strlen(codec->format_parameters) < AULA_SDP_MAX_FMTP_BYTES));
}

static aula_status aula_sdp_copy_capability_codec(aula_sdp_stored_media *target,
                                                     const aula_sdp_codec *codec,
                                                     size_t index, int video) {
  aula_status status;
  if (!aula_sdp_capability_codec_is_valid(codec, video)) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_sdp_add_codec(target, codec->kind, codec->payload_type,
                               codec->clock_rate, codec->channels);
  if (status != AULA_STATUS_OK) return status;
  target->declared_payload_types[codec->payload_type] = 1U;
  if (codec->format_parameters != NULL && codec->format_parameters[0] != '\0') {
    (void)memcpy(target->fmtp[index], codec->format_parameters,
                 strlen(codec->format_parameters) + 1U);
  }
  return AULA_STATUS_OK;
}

static void aula_sdp_copy_capabilities_transport(
    aula_sdp_stored_media *target, const aula_sdp_media_capabilities *capabilities,
    int video) {
  target->present = 1;
  target->is_video = video;
  target->value.rtp_port = capabilities->ports.rtp_port;
  target->value.rtcp_port = capabilities->ports.rtcp_port;
  target->value.rtcp_mux = capabilities->ports.rtcp_mux != 0;
  target->value.direction = capabilities->direction;
  target->value.codecs = target->codecs;
}

static aula_status aula_sdp_copy_capabilities_media(
    aula_sdp_stored_media *target, const aula_sdp_media_capabilities *capabilities,
    int video) {
  size_t index;
  if (target == NULL || !aula_sdp_capabilities_are_valid(capabilities))
    return AULA_STATUS_INVALID_ARGUMENT;
  aula_sdp_copy_capabilities_transport(target, capabilities, video);
  for (index = 0U; index < capabilities->codec_count; ++index) {
    aula_status status = aula_sdp_copy_capability_codec(target,
        &capabilities->codecs[index], index, video);
    if (status != AULA_STATUS_OK) return status;
  }
  return aula_sdp_validate_media(target, video);
}

aula_status aula_sdp_create_initial_offer(
    const aula_sdp_local_capabilities *capabilities, aula_sdp_session **out_offer) {
  aula_sdp_session *offer;
  aula_status status;
  if (capabilities == NULL || out_offer == NULL || *out_offer != NULL ||
      capabilities->local_address == NULL ||
      strlen(capabilities->local_address) >= AULA_SDP_MAX_ADDRESS_BYTES ||
      aula_sdp_validate_local_ipv4(capabilities->local_address) != AULA_STATUS_OK ||
      capabilities->remote_target_authorizer == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  offer = (aula_sdp_session *)calloc(1U, sizeof(*offer));
  if (offer == NULL) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  (void)memcpy(offer->connection_address, capabilities->local_address,
               strlen(capabilities->local_address) + 1U);
  offer->remote_target_authorizer = capabilities->remote_target_authorizer;
  offer->remote_target_context = capabilities->remote_target_context;
  status = aula_sdp_copy_capabilities_media(&offer->video, &capabilities->video, 1);
  if (status == AULA_STATUS_OK) {
    status = aula_sdp_copy_capabilities_media(&offer->audio, &capabilities->audio, 0);
  }
  if (status != AULA_STATUS_OK) {
    aula_sdp_session_destroy(offer);
    return status;
  }
  *out_offer = offer;
  return AULA_STATUS_OK;
}

aula_status aula_sdp_create_initial_srtp_offer(
    const aula_sdp_local_capabilities *capabilities,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_offer) {
  aula_status status;
  if ((video_srtp == NULL && audio_srtp == NULL) || out_offer == NULL || *out_offer != NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_sdp_create_initial_offer(capabilities, out_offer);
  if (status != AULA_STATUS_OK) return status;
  if (video_srtp != NULL) status = aula_sdp_assign_srtp_policy(&(*out_offer)->video, video_srtp);
  if (status == AULA_STATUS_OK && audio_srtp != NULL) {
    status = aula_sdp_assign_srtp_policy(&(*out_offer)->audio, audio_srtp);
  }
  if (status != AULA_STATUS_OK) {
    aula_sdp_session_destroy(*out_offer);
    *out_offer = NULL;
  }
  return status;
}

static aula_sdp_direction aula_sdp_answer_direction(aula_sdp_direction offer) {
  if (offer == AULA_SDP_SENDONLY) {
    return AULA_SDP_RECVONLY;
  }
  if (offer == AULA_SDP_RECVONLY) {
    return AULA_SDP_SENDONLY;
  }
  return offer;
}

int aula_sdp_answer_direction_is_compatible(aula_sdp_direction offer,
                                             aula_sdp_direction answer) {
  if (offer == AULA_SDP_SENDRECV) {
    return answer >= AULA_SDP_SENDRECV && answer <= AULA_SDP_INACTIVE;
  }
  if (offer == AULA_SDP_SENDONLY) {
    return answer == AULA_SDP_RECVONLY || answer == AULA_SDP_INACTIVE;
  }
  if (offer == AULA_SDP_RECVONLY) {
    return answer == AULA_SDP_SENDONLY || answer == AULA_SDP_INACTIVE;
  }
  return offer == AULA_SDP_INACTIVE && answer == AULA_SDP_INACTIVE;
}

static aula_status aula_sdp_copy_selected_codec(aula_sdp_stored_media *target,
                                                  const aula_sdp_stored_media *source,
                                                  aula_sdp_codec_kind kind) {
  size_t index;
  for (index = 0U; index < source->value.codec_count; ++index) {
    if (source->codecs[index].kind == kind) {
      aula_status status = aula_sdp_add_codec(target, kind, source->codecs[index].payload_type,
                                                source->codecs[index].clock_rate,
                                                source->codecs[index].channels);
      if (status == AULA_STATUS_OK && source->codecs[index].format_parameters[0] != '\0') {
        size_t target_index = target->value.codec_count - 1U;
        (void)memcpy(target->fmtp[target_index], source->codecs[index].format_parameters,
                     strlen(source->codecs[index].format_parameters) + 1U);
      }
      return status;
    }
  }
  return AULA_STATUS_UNSUPPORTED;
}

static void aula_sdp_copy_transport(aula_sdp_stored_media *target,
                                     const aula_sdp_stored_media *source) {
  target->present = 1;
  target->is_video = source->is_video;
  target->value.rtp_port = source->value.rtp_port;
  target->value.rtcp_port = source->value.rtcp_port;
  target->value.rtcp_mux = source->value.rtcp_mux;
  target->value.profile = source->value.profile;
  target->value.direction = aula_sdp_answer_direction(source->value.direction);
  target->value.codecs = target->codecs;
  target->value.codec_count = 0U;
}

aula_status aula_sdp_select_answer(const aula_sdp_session *offer,
                                     aula_sdp_session **out_answer) {
  aula_sdp_session *answer;
  aula_status status;
  if (offer == NULL || out_answer == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_answer = NULL;
  status = aula_sdp_validate_media(&offer->video, 1);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  status = aula_sdp_validate_media(&offer->audio, 0);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  if (offer->video.value.profile == AULA_SDP_MEDIA_PROFILE_RTP_SAVP ||
      offer->audio.value.profile == AULA_SDP_MEDIA_PROFILE_RTP_SAVP) {
    return AULA_STATUS_UNSUPPORTED;
  }
  answer = (aula_sdp_session *)calloc(1U, sizeof(*answer));
  if (answer == NULL) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  (void)memcpy(answer->connection_address, offer->connection_address,
               sizeof(answer->connection_address));
  answer->remote_target_authorizer = offer->remote_target_authorizer;
  answer->remote_target_context = offer->remote_target_context;
  aula_sdp_copy_transport(&answer->video, &offer->video);
  aula_sdp_copy_transport(&answer->audio, &offer->audio);
  status = aula_sdp_copy_selected_codec(&answer->video, &offer->video, AULA_SDP_CODEC_H264);
  if (status == AULA_STATUS_OK) {
    if (aula_sdp_media_has_kind(&offer->audio, AULA_SDP_CODEC_PCMU)) {
      status = aula_sdp_copy_selected_codec(&answer->audio, &offer->audio, AULA_SDP_CODEC_PCMU);
    } else {
      status = aula_sdp_copy_selected_codec(&answer->audio, &offer->audio, AULA_SDP_CODEC_PCMA);
    }
  }
  if (status == AULA_STATUS_OK &&
      aula_sdp_media_has_kind(&offer->audio,
                               AULA_SDP_CODEC_TELEPHONE_EVENT)) {
    status = aula_sdp_copy_selected_codec(&answer->audio, &offer->audio,
                                           AULA_SDP_CODEC_TELEPHONE_EVENT);
  }
  if (status != AULA_STATUS_OK) {
    aula_sdp_session_destroy(answer);
    return status;
  }
  *out_answer = answer;
  return AULA_STATUS_OK;
}

static aula_status aula_sdp_copy_policy_codec(aula_sdp_stored_media *target,
                                                 const aula_sdp_stored_media *offer,
                                                 aula_sdp_codec_kind kind,
                                                 int allowed) {
  if (!allowed) return AULA_STATUS_UNSUPPORTED;
  return aula_sdp_copy_selected_codec(target, offer, kind);
}

static int aula_sdp_policy_is_valid(const aula_sdp_answer_policy *policy) {
  return policy != NULL && aula_sdp_valid_assignment(&policy->video_ports) &&
         aula_sdp_valid_assignment(&policy->audio_ports) &&
         policy->video_direction >= AULA_SDP_SENDRECV &&
         policy->video_direction <= AULA_SDP_INACTIVE &&
         policy->audio_direction >= AULA_SDP_SENDRECV &&
         policy->audio_direction <= AULA_SDP_INACTIVE &&
         policy->local_address != NULL &&
         strlen(policy->local_address) < AULA_SDP_MAX_ADDRESS_BYTES &&
         aula_sdp_validate_local_ipv4(policy->local_address) == AULA_STATUS_OK &&
         policy->remote_target_authorizer != NULL;
}

static void aula_sdp_apply_policy_media(aula_sdp_stored_media *media,
                                         const aula_sdp_port_assignment *ports,
                                         aula_sdp_direction direction) {
  media->value.rtp_port = ports->rtp_port;
  media->value.rtcp_port = ports->rtcp_port;
  media->value.rtcp_mux = ports->rtcp_mux != 0;
  media->value.direction = direction;
}

static aula_status aula_sdp_apply_policy_srtp(
    aula_sdp_stored_media *answer, const aula_sdp_stored_media *offer,
    const aula_sdp_srtp_policy *policy) {
  if (answer == NULL || offer == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (offer->value.profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP) return AULA_STATUS_OK;
  if (offer->value.profile != AULA_SDP_MEDIA_PROFILE_RTP_SAVP) return AULA_STATUS_INVALID_DATA;
  if (policy == NULL) return AULA_STATUS_UNSUPPORTED;
  {
    aula_status status = aula_sdp_assign_srtp_policy(answer, policy);
    if (status == AULA_STATUS_OK) answer->srtp.crypto_tag = offer->srtp.crypto_tag;
    return status;
  }
}

static aula_status aula_sdp_select_policy_audio(aula_sdp_stored_media *target,
                                                   const aula_sdp_stored_media *offer,
                                                   const aula_sdp_answer_policy *policy) {
  if (policy->allow_pcmu && aula_sdp_media_has_kind(offer, AULA_SDP_CODEC_PCMU))
    return aula_sdp_copy_selected_codec(target, offer, AULA_SDP_CODEC_PCMU);
  if (policy->allow_pcma && aula_sdp_media_has_kind(offer, AULA_SDP_CODEC_PCMA))
    return aula_sdp_copy_selected_codec(target, offer, AULA_SDP_CODEC_PCMA);
  return AULA_STATUS_UNSUPPORTED;
}

static aula_status aula_sdp_select_policy_codecs(aula_sdp_session *answer,
                                                    const aula_sdp_session *offer,
                                                    const aula_sdp_answer_policy *policy) {
  aula_status status = aula_sdp_copy_policy_codec(&answer->video, &offer->video,
                                                     AULA_SDP_CODEC_H264, 1);
  if (status == AULA_STATUS_OK) status = aula_sdp_select_policy_audio(&answer->audio,
      &offer->audio, policy);
  if (status == AULA_STATUS_OK && policy->allow_telephone_event &&
      aula_sdp_media_has_kind(&offer->audio, AULA_SDP_CODEC_TELEPHONE_EVENT)) {
    status = aula_sdp_copy_policy_codec(&answer->audio, &offer->audio,
        AULA_SDP_CODEC_TELEPHONE_EVENT, policy->allow_telephone_event);
  }
  return status;
}

static aula_status aula_sdp_validate_policy_offer(
    const aula_sdp_session *offer, const aula_sdp_answer_policy *policy) {
  aula_status status;
  status = aula_sdp_validate_media(&offer->video, 1);
  if (status != AULA_STATUS_OK) return status;
  status = aula_sdp_validate_media(&offer->audio, 0);
  if (status != AULA_STATUS_OK) return status;
  if (!policy->allow_h264_packetization_mode_1) return AULA_STATUS_UNSUPPORTED;
  if (!aula_sdp_answer_direction_is_compatible(offer->video.value.direction,
                                                policy->video_direction) ||
      !aula_sdp_answer_direction_is_compatible(offer->audio.value.direction,
                                                policy->audio_direction))
    return AULA_STATUS_UNSUPPORTED;
  return AULA_STATUS_OK;
}

static aula_sdp_session *aula_sdp_create_policy_answer(
    const aula_sdp_session *offer, const aula_sdp_answer_policy *policy) {
  aula_sdp_session *answer = (aula_sdp_session *)calloc(1U, sizeof(*answer));
  if (answer == NULL) return NULL;
  (void)memcpy(answer->connection_address, policy->local_address,
               strlen(policy->local_address) + 1U);
  answer->remote_target_authorizer = policy->remote_target_authorizer;
  answer->remote_target_context = policy->remote_target_context;
  aula_sdp_copy_transport(&answer->video, &offer->video);
  aula_sdp_copy_transport(&answer->audio, &offer->audio);
  aula_sdp_apply_policy_media(&answer->video, &policy->video_ports, policy->video_direction);
  aula_sdp_apply_policy_media(&answer->audio, &policy->audio_ports, policy->audio_direction);
  return answer;
}

static aula_status aula_sdp_complete_policy_answer(
    aula_sdp_session *answer, const aula_sdp_session *offer,
    const aula_sdp_answer_policy *policy, const aula_sdp_srtp_policy *video_srtp,
    const aula_sdp_srtp_policy *audio_srtp) {
  aula_status status = aula_sdp_apply_policy_srtp(&answer->video, &offer->video,
                                                     video_srtp);
  if (status == AULA_STATUS_OK)
    status = aula_sdp_apply_policy_srtp(&answer->audio, &offer->audio, audio_srtp);
  if (status == AULA_STATUS_OK)
    status = aula_sdp_select_policy_codecs(answer, offer, policy);
  if (status == AULA_STATUS_OK) status = aula_sdp_validate_media(&answer->video, 1);
  if (status == AULA_STATUS_OK) status = aula_sdp_validate_media(&answer->audio, 0);
  return status;
}

aula_status aula_sdp_select_answer_with_srtp_policy(
    const aula_sdp_session *offer, const aula_sdp_answer_policy *policy,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_answer) {
  aula_sdp_session *answer;
  aula_status status;
  if (offer == NULL || out_answer == NULL || *out_answer != NULL ||
      !aula_sdp_policy_is_valid(policy)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_sdp_validate_policy_offer(offer, policy);
  if (status != AULA_STATUS_OK) return status;
  answer = aula_sdp_create_policy_answer(offer, policy);
  if (answer == NULL) return AULA_STATUS_INTERNAL_ERROR;
  status = aula_sdp_complete_policy_answer(answer, offer, policy, video_srtp, audio_srtp);
  if (status != AULA_STATUS_OK) {
    aula_sdp_session_destroy(answer);
    return status;
  }
  *out_answer = answer;
  return AULA_STATUS_OK;
}

aula_status aula_sdp_select_answer_with_policy(const aula_sdp_session *offer,
                                                 const aula_sdp_answer_policy *policy,
                                                 aula_sdp_session **out_answer) {
  return aula_sdp_select_answer_with_srtp_policy(offer, policy, NULL, NULL, out_answer);
}
