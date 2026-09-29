#ifndef AULA_SIPD_SDP_INTERNAL_H
#define AULA_SIPD_SDP_INTERNAL_H

#include "aula_sipd/sdp.h"
#include "negotiated_private.h"
#include "srtp_limits.h"

#define AULA_SDP_MAX_LINE_BYTES 512U
#define AULA_SDP_MAX_CODECS 3U
#define AULA_SDP_MAX_FMTP_BYTES 160U
#define AULA_SDP_MAX_ADDRESS_BYTES 64U

typedef struct aula_sdp_stored_srtp {
  int present;
  uint8_t crypto_tag;
  uint64_t lifetime;
  aula_sdp_srtp_material material;
} aula_sdp_stored_srtp;

typedef struct aula_sdp_stored_media {
  aula_sdp_media value;
  aula_sdp_codec codecs[AULA_SDP_MAX_CODECS];
  char fmtp[AULA_SDP_MAX_CODECS][AULA_SDP_MAX_FMTP_BYTES];
  uint8_t declared_payload_types[128];
  uint8_t mapped_payload_types[128];
  uint8_t feedback_payload[128];
  uint8_t feedback_wildcard;
  char rtcp_address[AULA_SDP_MAX_ADDRESS_BYTES];
  aula_sdp_stored_srtp srtp;
  int present;
  int is_video;
  int direction_explicit;
} aula_sdp_stored_media;

struct aula_sdp_session {
  aula_sdp_stored_media video;
  aula_sdp_stored_media audio;
  char connection_address[AULA_SDP_MAX_ADDRESS_BYTES];
  aula_sdp_media_target_authorizer remote_target_authorizer;
  void *remote_target_context;
};

aula_status aula_sdp_parse_feedback(aula_sdp_stored_media *media, char *value);
uint32_t aula_sdp_media_feedback(const aula_sdp_stored_media *media,
                                  uint8_t payload);
aula_status aula_sdp_serialize_feedback(const aula_sdp_stored_media *media,
                                          aula_mutable_bytes *output);

void aula_sdp_secure_zero(void *memory, size_t length);
int aula_sdp_srtp_is_available(void);
int aula_sdp_srtp_policy_is_valid(const aula_sdp_srtp_policy *policy);
aula_status aula_sdp_assign_srtp_policy(aula_sdp_stored_media *media,
                                          const aula_sdp_srtp_policy *policy);
aula_status aula_sdp_parse_crypto(aula_sdp_stored_media *media, char *value);
void aula_sdp_log_crypto_diagnostics(aula_bytes input, aula_status status);
void aula_sdp_log_codec_diagnostics(const aula_sdp_session *session,
                                     int strict_offer, int remote);
int aula_sdp_validation_reason(const aula_sdp_stored_media *media,
                                int strict_offer, int video);

struct aula_sdp_negotiated_session {
  aula_sdp_negotiated_media video;
  aula_sdp_negotiated_media audio;
  aula_sdp_receive_media receive_video;
  aula_sdp_receive_media receive_audio;
  aula_sdp_srtp_lifetimes video_srtp_lifetimes;
  aula_sdp_srtp_lifetimes audio_srtp_lifetimes;
};

int aula_sdp_ascii_equal(const char *left, const char *right);
int aula_sdp_parse_u32(const char *text, uint32_t *out_value);
int aula_sdp_parse_u16(const char *text, uint16_t *out_value);
int aula_sdp_parse_payload(const char *text, uint8_t *out_value);
int aula_sdp_split_words(char *text, char **words, size_t maximum, size_t *out_count);
int aula_sdp_valid_text(const char *text);
aula_status aula_sdp_parse_ipv4(const char *text, unsigned int octets[4]);
aula_status aula_sdp_validate_local_ipv4(const char *text);
aula_status aula_sdp_authorize_remote_ipv4(const aula_sdp_session *local,
                                             const char *text, uint16_t port, int is_rtcp);
aula_sdp_codec *aula_sdp_find_codec(aula_sdp_stored_media *media, uint8_t payload_type);
const aula_sdp_codec *aula_sdp_find_codec_const(const aula_sdp_stored_media *media,
                                                  uint8_t payload_type);
aula_status aula_sdp_add_codec(aula_sdp_stored_media *media, aula_sdp_codec_kind kind,
                                 uint8_t payload_type, uint32_t clock_rate, uint8_t channels);
aula_status aula_sdp_parse_media(aula_sdp_session *session, char *value,
                                   aula_sdp_stored_media **out_media);
aula_status aula_sdp_parse_rtpmap(aula_sdp_stored_media *media, char *value);
aula_status aula_sdp_parse_fmtp(aula_sdp_stored_media *media, char *value);
int aula_sdp_h264_fmtp_is_compatible(const char *fmtp);
int aula_sdp_media_has_kind(const aula_sdp_stored_media *media, aula_sdp_codec_kind kind);
aula_status aula_sdp_validate_media(const aula_sdp_stored_media *media, int is_video);
aula_status aula_sdp_validate_answer_media(const aula_sdp_stored_media *media, int is_video);
aula_status aula_sdp_set_direction(aula_sdp_stored_media *media,
                                     const char *value,
                                     aula_sdp_direction *session_direction,
                                     int *has_session_direction);
int aula_sdp_answer_direction_is_compatible(aula_sdp_direction offer,
                                             aula_sdp_direction answer);
aula_status aula_sdp_select_answer_with_srtp_policy(
    const aula_sdp_session *offer, const aula_sdp_answer_policy *policy,
    const aula_sdp_srtp_policy *video_srtp, const aula_sdp_srtp_policy *audio_srtp,
    aula_sdp_session **out_answer);
#endif
