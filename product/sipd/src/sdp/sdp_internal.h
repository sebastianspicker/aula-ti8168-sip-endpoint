#ifndef LS200_SIPD_SDP_INTERNAL_H
#define LS200_SIPD_SDP_INTERNAL_H

#include "ls200_sipd/sdp.h"
#include "negotiated_private.h"
#include "srtp_limits.h"

#define LS200_SDP_MAX_LINE_BYTES 512U
#define LS200_SDP_MAX_CODECS 3U
#define LS200_SDP_MAX_FMTP_BYTES 160U
#define LS200_SDP_MAX_ADDRESS_BYTES 64U

typedef struct ls200_sdp_stored_srtp {
  int present;
  uint8_t crypto_tag;
  uint64_t lifetime;
  ls200_sdp_srtp_material material;
} ls200_sdp_stored_srtp;

typedef struct ls200_sdp_stored_media {
  ls200_sdp_media value;
  ls200_sdp_codec codecs[LS200_SDP_MAX_CODECS];
  char fmtp[LS200_SDP_MAX_CODECS][LS200_SDP_MAX_FMTP_BYTES];
  uint8_t declared_payload_types[128];
  uint8_t mapped_payload_types[128];
  uint8_t feedback_payload[128];
  uint8_t feedback_wildcard;
  char rtcp_address[LS200_SDP_MAX_ADDRESS_BYTES];
  ls200_sdp_stored_srtp srtp;
  int present;
  int is_video;
  int direction_explicit;
} ls200_sdp_stored_media;

struct ls200_sdp_session {
  ls200_sdp_stored_media video;
  ls200_sdp_stored_media audio;
  char connection_address[LS200_SDP_MAX_ADDRESS_BYTES];
  ls200_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
};

ls200_status ls200_sdp_parse_feedback(ls200_sdp_stored_media *media, char *value);
uint32_t ls200_sdp_media_feedback(const ls200_sdp_stored_media *media,
                                  uint8_t payload);
ls200_status ls200_sdp_serialize_feedback(const ls200_sdp_stored_media *media,
                                          ls200_mutable_bytes *output);

void ls200_sdp_secure_zero(void *memory, size_t length);
int ls200_sdp_srtp_is_available(void);
int ls200_sdp_srtp_policy_is_valid(const ls200_sdp_srtp_policy *policy);
ls200_status ls200_sdp_assign_srtp_policy(ls200_sdp_stored_media *media,
                                          const ls200_sdp_srtp_policy *policy);
ls200_status ls200_sdp_parse_crypto(ls200_sdp_stored_media *media, char *value);
void ls200_sdp_log_crypto_diagnostics(ls200_bytes input, ls200_status status);
void ls200_sdp_log_codec_diagnostics(const ls200_sdp_session *session,
                                     int strict_offer, int remote);
int ls200_sdp_validation_reason(const ls200_sdp_stored_media *media,
                                int strict_offer, int video);

struct ls200_sdp_negotiated_session {
  ls200_sdp_negotiated_media video;
  ls200_sdp_negotiated_media audio;
  ls200_sdp_receive_media receive_video;
  ls200_sdp_receive_media receive_audio;
  ls200_sdp_srtp_lifetimes video_srtp_lifetimes;
  ls200_sdp_srtp_lifetimes audio_srtp_lifetimes;
};

int ls200_sdp_ascii_equal(const char *left, const char *right);
int ls200_sdp_parse_u32(const char *text, uint32_t *out_value);
int ls200_sdp_parse_u16(const char *text, uint16_t *out_value);
int ls200_sdp_parse_payload(const char *text, uint8_t *out_value);
int ls200_sdp_split_words(char *text, char **words, size_t maximum, size_t *out_count);
int ls200_sdp_valid_text(const char *text);
ls200_status ls200_sdp_parse_ipv4(const char *text, unsigned int octets[4]);
ls200_status ls200_sdp_validate_local_ipv4(const char *text);
ls200_status ls200_sdp_authorize_remote_ipv4(const ls200_sdp_session *local,
                                             const char *text, uint16_t port, int is_rtcp);
ls200_sdp_codec *ls200_sdp_find_codec(ls200_sdp_stored_media *media, uint8_t payload_type);
const ls200_sdp_codec *ls200_sdp_find_codec_const(const ls200_sdp_stored_media *media,
                                                  uint8_t payload_type);
ls200_status ls200_sdp_add_codec(ls200_sdp_stored_media *media, ls200_sdp_codec_kind kind,
                                 uint8_t payload_type, uint32_t clock_rate, uint8_t channels);
ls200_status ls200_sdp_parse_media(ls200_sdp_session *session, char *value,
                                   ls200_sdp_stored_media **out_media);
ls200_status ls200_sdp_parse_rtpmap(ls200_sdp_stored_media *media, char *value);
ls200_status ls200_sdp_parse_fmtp(ls200_sdp_stored_media *media, char *value);
int ls200_sdp_h264_fmtp_is_compatible(const char *fmtp);
int ls200_sdp_media_has_kind(const ls200_sdp_stored_media *media, ls200_sdp_codec_kind kind);
ls200_status ls200_sdp_validate_media(const ls200_sdp_stored_media *media, int is_video);
ls200_status ls200_sdp_validate_answer_media(const ls200_sdp_stored_media *media, int is_video);
ls200_status ls200_sdp_set_direction(ls200_sdp_stored_media *media,
                                     const char *value,
                                     ls200_sdp_direction *session_direction,
                                     int *has_session_direction);
int ls200_sdp_answer_direction_is_compatible(ls200_sdp_direction offer,
                                             ls200_sdp_direction answer);
ls200_status ls200_sdp_select_answer_with_srtp_policy(
    const ls200_sdp_session *offer, const ls200_sdp_answer_policy *policy,
    const ls200_sdp_srtp_policy *video_srtp, const ls200_sdp_srtp_policy *audio_srtp,
    ls200_sdp_session **out_answer);
#endif
