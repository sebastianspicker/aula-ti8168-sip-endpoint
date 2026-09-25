#ifndef LS200_SIPD_SDP_H
#define LS200_SIPD_SDP_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_sdp_direction {
  LS200_SDP_SENDRECV = 0,
  LS200_SDP_SENDONLY,
  LS200_SDP_RECVONLY,
  LS200_SDP_INACTIVE
} ls200_sdp_direction;

typedef enum ls200_sdp_codec_kind {
  LS200_SDP_CODEC_H264 = 0,
  LS200_SDP_CODEC_PCMU,
  LS200_SDP_CODEC_PCMA,
  LS200_SDP_CODEC_TELEPHONE_EVENT
} ls200_sdp_codec_kind;

typedef struct ls200_sdp_codec {
  ls200_sdp_codec_kind kind;
  uint8_t payload_type;
  uint32_t clock_rate;
  uint8_t channels;
  const char *format_parameters;
} ls200_sdp_codec;

#define LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES 30U

typedef enum ls200_sdp_media_profile {
  LS200_SDP_MEDIA_PROFILE_RTP_AVP = 0,
  LS200_SDP_MEDIA_PROFILE_RTP_SAVP
} ls200_sdp_media_profile;

/* This is binary keying material, never an SDP inline string. */
typedef struct ls200_sdp_srtp_material {
  uint8_t master_key_salt[LS200_SDP_SRTP_MASTER_KEY_SALT_BYTES];
} ls200_sdp_srtp_material;

/* A supplied policy represents the material this endpoint will send with.
 * The tag is restricted to the SDP crypto tag range 1 through 255. */
typedef struct ls200_sdp_srtp_policy {
  uint8_t crypto_tag;
  ls200_sdp_srtp_material outbound_material;
} ls200_sdp_srtp_policy;

typedef struct ls200_sdp_media {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  ls200_sdp_direction direction;
  const ls200_sdp_codec *codecs;
  size_t codec_count;
  int rtcp_mux;
  ls200_sdp_media_profile profile;
} ls200_sdp_media;

typedef struct ls200_sdp_port_assignment {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  int rtcp_mux;
} ls200_sdp_port_assignment;

/* Authorize each negotiated remote media target after syntactic SDP parsing.
 * Implementations must bind the address to signaling/account policy; a broad
 * "any public address" decision is not a safe production policy. */
typedef ls200_status (*ls200_sdp_media_target_authorizer)(
    void *context, const char *address, uint16_t port, int is_rtcp);

typedef struct ls200_sdp_answer_policy {
  const char *local_address;
  ls200_sdp_port_assignment video_ports;
  ls200_sdp_port_assignment audio_ports;
  ls200_sdp_direction video_direction;
  ls200_sdp_direction audio_direction;
  int allow_pcmu;
  int allow_pcma;
  int allow_telephone_event;
  int allow_h264_packetization_mode_1;
  ls200_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
} ls200_sdp_answer_policy;

typedef struct ls200_sdp_session ls200_sdp_session;

/* Capabilities are explicit because an SDP offer advertises already-bound
 * receive targets.  The codec array is copied when an offer is created. */
typedef struct ls200_sdp_media_capabilities {
  ls200_sdp_port_assignment ports;
  ls200_sdp_direction direction;
  const ls200_sdp_codec *codecs;
  size_t codec_count;
} ls200_sdp_media_capabilities;

typedef struct ls200_sdp_local_capabilities {
  const char *local_address;
  ls200_sdp_media_capabilities video;
  ls200_sdp_media_capabilities audio;
  ls200_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
} ls200_sdp_local_capabilities;

#define LS200_SDP_MAX_SELECTED_CODECS 3U
#define LS200_SDP_MAX_NEGOTIATED_FMTP_BYTES 160U
#define LS200_SDP_MAX_H264_PARAMETER_SETS_BYTES 128U

typedef struct ls200_sdp_negotiated_codec {
  ls200_sdp_codec_kind kind;
  uint8_t payload_type;
  uint32_t clock_rate;
  uint8_t channels;
  char format_parameters[LS200_SDP_MAX_NEGOTIATED_FMTP_BYTES];
} ls200_sdp_negotiated_codec;

typedef struct ls200_sdp_h264_evidence {
  int present;
  int packetization_mode;
  int has_parameter_sets;
  char profile_level_id[7];
  char parameter_sets[LS200_SDP_MAX_H264_PARAMETER_SETS_BYTES];
} ls200_sdp_h264_evidence;

typedef struct ls200_sdp_negotiated_media {
  char remote_rtp_address[64];
  char remote_rtcp_address[64];
  uint16_t local_rtp_port;
  uint16_t local_rtcp_port;
  uint16_t remote_rtp_port;
  uint16_t remote_rtcp_port;
  int local_rtcp_mux;
  int remote_rtcp_mux;
  ls200_sdp_direction local_direction;
  ls200_sdp_direction remote_direction;
  ls200_sdp_negotiated_codec codecs[LS200_SDP_MAX_SELECTED_CODECS];
  size_t codec_count;
  ls200_sdp_h264_evidence h264;
  int srtp_enabled;
  uint8_t local_outbound_srtp_tag;
  uint8_t remote_inbound_srtp_tag;
  ls200_sdp_srtp_material local_outbound_srtp;
  ls200_sdp_srtp_material remote_inbound_srtp;
} ls200_sdp_negotiated_media;

typedef struct ls200_sdp_negotiated_session ls200_sdp_negotiated_session;

ls200_status ls200_sdp_parse_offer(ls200_bytes input, ls200_sdp_session **out_session);
ls200_status ls200_sdp_parse_answer(ls200_bytes input, ls200_sdp_session **out_session);
ls200_status ls200_sdp_create_initial_offer(const ls200_sdp_local_capabilities *capabilities,
                                            ls200_sdp_session **out_offer);
ls200_status ls200_sdp_create_initial_srtp_offer(
    const ls200_sdp_local_capabilities *capabilities,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_offer);
ls200_status ls200_sdp_validate_remote_answer(
    const ls200_sdp_session *local_offer, const ls200_sdp_session *remote_answer,
    ls200_sdp_negotiated_session **out_negotiated);
ls200_status ls200_sdp_negotiate_remote_answer(
    const ls200_sdp_session *local_offer, ls200_bytes remote_answer,
    ls200_sdp_negotiated_session **out_negotiated);
ls200_status ls200_sdp_answer_remote_offer(
    const ls200_sdp_session *remote_offer, const ls200_sdp_answer_policy *policy,
    ls200_sdp_session **out_answer, ls200_sdp_negotiated_session **out_negotiated);
ls200_status ls200_sdp_answer_remote_srtp_offer(
    const ls200_sdp_session *remote_offer, const ls200_sdp_answer_policy *policy,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_answer, ls200_sdp_negotiated_session **out_negotiated);
ls200_status ls200_sdp_select_answer(const ls200_sdp_session *offer,
                                     ls200_sdp_session **out_answer);
ls200_status ls200_sdp_select_answer_with_policy(const ls200_sdp_session *offer,
                                                 const ls200_sdp_answer_policy *policy,
                                                 ls200_sdp_session **out_answer);
ls200_status ls200_sdp_serialize(const ls200_sdp_session *session,
                                 ls200_mutable_bytes *output);
const ls200_sdp_media *ls200_sdp_get_video(const ls200_sdp_session *session);
const ls200_sdp_media *ls200_sdp_get_audio(const ls200_sdp_session *session);
void ls200_sdp_session_destroy(ls200_sdp_session *session);
const ls200_sdp_negotiated_media *ls200_sdp_negotiated_get_video(
    const ls200_sdp_negotiated_session *session);
const ls200_sdp_negotiated_media *ls200_sdp_negotiated_get_audio(
    const ls200_sdp_negotiated_session *session);
void ls200_sdp_negotiated_session_destroy(ls200_sdp_negotiated_session *session);

#ifdef __cplusplus
}
#endif

#endif
