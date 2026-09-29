#ifndef AULA_SIPD_SDP_H
#define AULA_SIPD_SDP_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_sdp_direction {
  AULA_SDP_SENDRECV = 0,
  AULA_SDP_SENDONLY,
  AULA_SDP_RECVONLY,
  AULA_SDP_INACTIVE
} aula_sdp_direction;

typedef enum aula_sdp_codec_kind {
  AULA_SDP_CODEC_H264 = 0,
  AULA_SDP_CODEC_PCMU,
  AULA_SDP_CODEC_PCMA,
  AULA_SDP_CODEC_TELEPHONE_EVENT
} aula_sdp_codec_kind;

typedef struct aula_sdp_codec {
  aula_sdp_codec_kind kind;
  uint8_t payload_type;
  uint32_t clock_rate;
  uint8_t channels;
  const char *format_parameters;
} aula_sdp_codec;

#define AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES 30U

typedef enum aula_sdp_media_profile {
  AULA_SDP_MEDIA_PROFILE_RTP_AVP = 0,
  AULA_SDP_MEDIA_PROFILE_RTP_SAVP
} aula_sdp_media_profile;

/* This is binary keying material, never an SDP inline string. */
typedef struct aula_sdp_srtp_material {
  uint8_t master_key_salt[AULA_SDP_SRTP_MASTER_KEY_SALT_BYTES];
} aula_sdp_srtp_material;

/* A supplied policy represents the material this endpoint will send with.
 * The tag is restricted to the SDP crypto tag range 1 through 255. */
typedef struct aula_sdp_srtp_policy {
  uint8_t crypto_tag;
  aula_sdp_srtp_material outbound_material;
} aula_sdp_srtp_policy;

typedef struct aula_sdp_media {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  aula_sdp_direction direction;
  const aula_sdp_codec *codecs;
  size_t codec_count;
  int rtcp_mux;
  aula_sdp_media_profile profile;
} aula_sdp_media;

typedef struct aula_sdp_port_assignment {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  int rtcp_mux;
} aula_sdp_port_assignment;

/* Authorize each negotiated remote media target after syntactic SDP parsing.
 * Implementations must bind the address to signaling/account policy; a broad
 * "any public address" decision is not a safe production policy. */
typedef aula_status (*aula_sdp_media_target_authorizer)(
    void *context, const char *address, uint16_t port, int is_rtcp);

typedef struct aula_sdp_answer_policy {
  const char *local_address;
  aula_sdp_port_assignment video_ports;
  aula_sdp_port_assignment audio_ports;
  aula_sdp_direction video_direction;
  aula_sdp_direction audio_direction;
  int allow_pcmu;
  int allow_pcma;
  int allow_telephone_event;
  int allow_h264_packetization_mode_1;
  aula_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
} aula_sdp_answer_policy;

typedef struct aula_sdp_session aula_sdp_session;

/* Capabilities are explicit because an SDP offer advertises already-bound
 * receive targets.  The codec array is copied when an offer is created. */
typedef struct aula_sdp_media_capabilities {
  aula_sdp_port_assignment ports;
  aula_sdp_direction direction;
  const aula_sdp_codec *codecs;
  size_t codec_count;
} aula_sdp_media_capabilities;

typedef struct aula_sdp_local_capabilities {
  const char *local_address;
  aula_sdp_media_capabilities video;
  aula_sdp_media_capabilities audio;
  aula_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
} aula_sdp_local_capabilities;

#define AULA_SDP_MAX_SELECTED_CODECS 3U
#define AULA_SDP_MAX_NEGOTIATED_FMTP_BYTES 160U
#define AULA_SDP_MAX_H264_PARAMETER_SETS_BYTES 128U

typedef struct aula_sdp_negotiated_codec {
  aula_sdp_codec_kind kind;
  uint8_t payload_type;
  uint32_t clock_rate;
  uint8_t channels;
  char format_parameters[AULA_SDP_MAX_NEGOTIATED_FMTP_BYTES];
} aula_sdp_negotiated_codec;

typedef struct aula_sdp_h264_evidence {
  int present;
  int packetization_mode;
  int has_parameter_sets;
  char profile_level_id[7];
  char parameter_sets[AULA_SDP_MAX_H264_PARAMETER_SETS_BYTES];
} aula_sdp_h264_evidence;

typedef struct aula_sdp_negotiated_media {
  char remote_rtp_address[64];
  char remote_rtcp_address[64];
  uint16_t local_rtp_port;
  uint16_t local_rtcp_port;
  uint16_t remote_rtp_port;
  uint16_t remote_rtcp_port;
  int local_rtcp_mux;
  int remote_rtcp_mux;
  aula_sdp_direction local_direction;
  aula_sdp_direction remote_direction;
  aula_sdp_negotiated_codec codecs[AULA_SDP_MAX_SELECTED_CODECS];
  size_t codec_count;
  aula_sdp_h264_evidence h264;
  int srtp_enabled;
  uint8_t local_outbound_srtp_tag;
  uint8_t remote_inbound_srtp_tag;
  aula_sdp_srtp_material local_outbound_srtp;
  aula_sdp_srtp_material remote_inbound_srtp;
} aula_sdp_negotiated_media;

typedef struct aula_sdp_negotiated_session aula_sdp_negotiated_session;

aula_status aula_sdp_parse_offer(aula_bytes input, aula_sdp_session **out_session);
aula_status aula_sdp_parse_answer(aula_bytes input, aula_sdp_session **out_session);
aula_status aula_sdp_create_initial_offer(const aula_sdp_local_capabilities *capabilities,
                                            aula_sdp_session **out_offer);
aula_status aula_sdp_create_initial_srtp_offer(
    const aula_sdp_local_capabilities *capabilities,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_offer);
aula_status aula_sdp_validate_remote_answer(
    const aula_sdp_session *local_offer, const aula_sdp_session *remote_answer,
    aula_sdp_negotiated_session **out_negotiated);
aula_status aula_sdp_negotiate_remote_answer(
    const aula_sdp_session *local_offer, aula_bytes remote_answer,
    aula_sdp_negotiated_session **out_negotiated);
aula_status aula_sdp_answer_remote_offer(
    const aula_sdp_session *remote_offer, const aula_sdp_answer_policy *policy,
    aula_sdp_session **out_answer, aula_sdp_negotiated_session **out_negotiated);
aula_status aula_sdp_answer_remote_srtp_offer(
    const aula_sdp_session *remote_offer, const aula_sdp_answer_policy *policy,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_answer, aula_sdp_negotiated_session **out_negotiated);
aula_status aula_sdp_select_answer(const aula_sdp_session *offer,
                                     aula_sdp_session **out_answer);
aula_status aula_sdp_select_answer_with_policy(const aula_sdp_session *offer,
                                                 const aula_sdp_answer_policy *policy,
                                                 aula_sdp_session **out_answer);
aula_status aula_sdp_serialize(const aula_sdp_session *session,
                                 aula_mutable_bytes *output);
const aula_sdp_media *aula_sdp_get_video(const aula_sdp_session *session);
const aula_sdp_media *aula_sdp_get_audio(const aula_sdp_session *session);
void aula_sdp_session_destroy(aula_sdp_session *session);
const aula_sdp_negotiated_media *aula_sdp_negotiated_get_video(
    const aula_sdp_negotiated_session *session);
const aula_sdp_negotiated_media *aula_sdp_negotiated_get_audio(
    const aula_sdp_negotiated_session *session);
void aula_sdp_negotiated_session_destroy(aula_sdp_negotiated_session *session);

#ifdef __cplusplus
}
#endif

#endif
