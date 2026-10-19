/* SDP offer/answer: parsing, serialization and answer negotiation through the
 * public API. Only plain RTP/AVP sessions are exercised; SRTP keying needs
 * the optional crypto backend and is out of scope here. Rejected inputs make
 * the parser print a one-line diagnostic on stderr; that is expected. */
#include "aula_sipd/sdp.h"
#include "aula_test.h"

#include <stdint.h>

#define SDP_VIDEO_FMTP "packetization-mode=1;profile-level-id=42e01f"

static aula_bytes text_bytes(const char *text) {
  aula_bytes bytes;
  bytes.data = (const uint8_t *)text;
  bytes.length = strlen(text);
  return bytes;
}

static const char valid_offer[] =
    "v=0\r\n"
    "o=- 1 1 IN IP4 192.168.1.10\r\n"
    "s=-\r\n"
    "c=IN IP4 192.168.1.10\r\n"
    "t=0 0\r\n"
    "m=video 40000 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n"
    "a=sendrecv\r\n"
    "m=audio 40002 RTP/AVP 0 101\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:101 0-15\r\n"
    "a=sendrecv\r\n";

static aula_sdp_session *parse_offer_text(const char *text) {
  aula_sdp_session *session = NULL;
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_OK);
  return session;
}

static void test_parse_valid_offer(void) {
  aula_sdp_session *session = parse_offer_text(valid_offer);
  const aula_sdp_media *video;
  const aula_sdp_media *audio;
  size_t index;
  int has_pcmu = 0;
  int has_event = 0;
  AULA_CHECK(session != NULL);
  if (session == NULL) return;
  video = aula_sdp_get_video(session);
  audio = aula_sdp_get_audio(session);
  AULA_CHECK(video != NULL && audio != NULL);
  if (video != NULL) {
    AULA_CHECK(video->rtp_port == 40000U);
    AULA_CHECK(video->rtcp_port == 40001U);
    AULA_CHECK(video->rtcp_mux == 0);
    AULA_CHECK(video->direction == AULA_SDP_SENDRECV);
    AULA_CHECK(video->profile == AULA_SDP_MEDIA_PROFILE_RTP_AVP);
    AULA_CHECK(video->codec_count == 1U);
    if (video->codec_count == 1U) {
      AULA_CHECK(video->codecs[0].kind == AULA_SDP_CODEC_H264);
      AULA_CHECK(video->codecs[0].payload_type == 96U);
      AULA_CHECK(video->codecs[0].clock_rate == 90000U);
    }
  }
  if (audio != NULL) {
    AULA_CHECK(audio->rtp_port == 40002U);
    AULA_CHECK(audio->rtcp_port == 40003U);
    AULA_CHECK(audio->direction == AULA_SDP_SENDRECV);
    for (index = 0U; index < audio->codec_count; ++index) {
      if (audio->codecs[index].kind == AULA_SDP_CODEC_PCMU) {
        has_pcmu = 1;
        AULA_CHECK(audio->codecs[index].payload_type == 0U);
        AULA_CHECK(audio->codecs[index].clock_rate == 8000U);
      }
      if (audio->codecs[index].kind == AULA_SDP_CODEC_TELEPHONE_EVENT) {
        has_event = 1;
        AULA_CHECK(audio->codecs[index].payload_type == 101U);
      }
    }
    AULA_CHECK(has_pcmu && has_event);
  }
  aula_sdp_session_destroy(session);
}

static void test_parse_accepts_lf_and_session_direction(void) {
  /* Bare LF line endings are tolerated, and a session-level direction
   * applies to media sections that do not set their own. */
  aula_sdp_session *session = parse_offer_text(
      "v=0\n"
      "o=- 1 1 IN IP4 192.168.1.10\n"
      "s=-\n"
      "c=IN IP4 192.168.1.10\n"
      "t=0 0\n"
      "a=recvonly\n"
      "m=video 40000 RTP/AVP 96\n"
      "a=rtpmap:96 H264/90000\n"
      "a=fmtp:96 " SDP_VIDEO_FMTP "\n"
      "m=audio 40002 RTP/AVP 8\n"
      "a=rtpmap:8 PCMA/8000\n"
      "a=sendonly\n");
  AULA_CHECK(session != NULL);
  if (session == NULL) return;
  AULA_CHECK(aula_sdp_get_video(session) != NULL &&
             aula_sdp_get_video(session)->direction == AULA_SDP_RECVONLY);
  AULA_CHECK(aula_sdp_get_audio(session) != NULL &&
             aula_sdp_get_audio(session)->direction == AULA_SDP_SENDONLY);
  aula_sdp_session_destroy(session);
}

static void test_parse_rejects_malformed_input(void) {
  aula_sdp_session *session = NULL;
  static const char header[] =
      "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\ns=-\r\n"
      "c=IN IP4 192.168.1.10\r\nt=0 0\r\n";
  static const char video[] =
      "m=video 40000 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n";
  static const char audio[] =
      "m=audio 40002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
  char text[1024];
  /* Argument checks. */
  AULA_CHECK_STATUS(aula_sdp_parse_offer((aula_bytes){NULL, 0U}, &session),
                    AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(""), &session),
                    AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(valid_offer), NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
  /* Not SDP at all, and a line without the "x=" shape. */
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes("hello"), &session),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK(session == NULL);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes("v=0\r\nbogus\r\n"),
                    &session), AULA_STATUS_INVALID_DATA);
  /* A bare carriage return is not a line ending. */
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes("v=0\ro=-\r"), &session),
                    AULA_STATUS_INVALID_DATA);
  /* Missing mandatory session fields. */
  (void)snprintf(text, sizeof(text), "o=- 1 1 IN IP4 192.168.1.10\r\ns=-\r\n"
                 "c=IN IP4 192.168.1.10\r\nt=0 0\r\n%s%s", video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  (void)snprintf(text, sizeof(text), "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\n"
                 "s=-\r\nt=0 0\r\n%s%s", video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* Only version 0 exists. */
  (void)snprintf(text, sizeof(text), "v=1\r\no=- 1 1 IN IP4 192.168.1.10\r\n"
                 "s=-\r\nc=IN IP4 192.168.1.10\r\nt=0 0\r\n%s%s", video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* Duplicate version line. */
  (void)snprintf(text, sizeof(text), "v=0\r\n%s%s%s", header, video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* IPv6 and malformed connection addresses. */
  (void)snprintf(text, sizeof(text), "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\n"
                 "s=-\r\nc=IN IP6 ::1\r\nt=0 0\r\n%s%s", video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  (void)snprintf(text, sizeof(text), "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\n"
                 "s=-\r\nc=IN IP4 300.1.1.1\r\nt=0 0\r\n%s%s", video, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK(session == NULL);
}

static void test_parse_rejects_bad_media(void) {
  aula_sdp_session *session = NULL;
  static const char header[] =
      "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\ns=-\r\n"
      "c=IN IP4 192.168.1.10\r\nt=0 0\r\n";
  static const char audio[] =
      "m=audio 40002 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n";
  char text[1024];
  /* An offer must carry both video and audio. */
  (void)snprintf(text, sizeof(text), "%s%s", header, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* An odd RTP port is refused. */
  (void)snprintf(text, sizeof(text),
                 "%sm=video 40001 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n%s", header, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* Port zero (a rejected stream) is refused. */
  (void)snprintf(text, sizeof(text),
                 "%sm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n%s", header, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_INVALID_DATA);
  /* A video section without H.264 is an unsupported offer, not bad data. */
  (void)snprintf(text, sizeof(text),
                 "%sm=video 40000 RTP/AVP 98\r\na=rtpmap:98 VP8/90000\r\n%s",
                 header, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_UNSUPPORTED);
  /* H.264 with packetization-mode 0 is not compatible. */
  (void)snprintf(text, sizeof(text),
                 "%sm=video 40000 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=0;profile-level-id=42e01f\r\n%s",
                 header, audio);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_UNSUPPORTED);
  /* Audio with neither PCMU nor PCMA. */
  (void)snprintf(text, sizeof(text),
                 "%sm=video 40000 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n"
                 "m=audio 40002 RTP/AVP 97\r\na=rtpmap:97 opus/48000/2\r\n",
                 header);
  AULA_CHECK_STATUS(aula_sdp_parse_offer(text_bytes(text), &session),
                    AULA_STATUS_UNSUPPORTED);
  AULA_CHECK(session == NULL);
}

static void test_serialize_round_trip(void) {
  aula_sdp_session *offer = parse_offer_text(valid_offer);
  aula_sdp_session *reparsed = NULL;
  uint8_t buffer[AULA_SIPD_MAX_SDP_BYTES];
  aula_mutable_bytes output = {buffer, sizeof(buffer), 0U};
  uint8_t tiny[8];
  aula_mutable_bytes small = {tiny, sizeof(tiny), 0U};
  AULA_CHECK(offer != NULL);
  if (offer == NULL) return;
  AULA_CHECK_STATUS(aula_sdp_serialize(offer, &output), AULA_STATUS_OK);
  AULA_CHECK(output.length > 0U && output.length <= sizeof(buffer));
  AULA_CHECK(output.length >= 3U && memcmp(buffer, "v=0", 3U) == 0);
  AULA_CHECK_STATUS(aula_sdp_parse_offer((aula_bytes){buffer, output.length},
                    &reparsed), AULA_STATUS_OK);
  if (reparsed != NULL) {
    const aula_sdp_media *video = aula_sdp_get_video(reparsed);
    const aula_sdp_media *audio = aula_sdp_get_audio(reparsed);
    AULA_CHECK(video != NULL && video->rtp_port == 40000U &&
               video->codec_count == 1U);
    AULA_CHECK(audio != NULL && audio->rtp_port == 40002U &&
               audio->direction == AULA_SDP_SENDRECV);
  }
  /* An output buffer that is too small is refused, not truncated. */
  AULA_CHECK(aula_sdp_serialize(offer, &small) != AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_sdp_serialize(NULL, &output),
                    AULA_STATUS_INVALID_ARGUMENT);
  aula_sdp_session_destroy(reparsed);
  aula_sdp_session_destroy(offer);
}

static aula_status accept_authorizer(void *context, const char *address,
                                     uint16_t port, int is_rtcp) {
  (void)context;
  (void)address;
  (void)port;
  (void)is_rtcp;
  return AULA_STATUS_OK;
}

static aula_sdp_answer_policy make_policy(void) {
  aula_sdp_answer_policy policy;
  (void)memset(&policy, 0, sizeof(policy));
  policy.local_address = "192.168.1.20";
  policy.video_ports.rtp_port = 50000U;
  policy.video_ports.rtcp_port = 50001U;
  policy.audio_ports.rtp_port = 50002U;
  policy.audio_ports.rtcp_port = 50003U;
  policy.video_direction = AULA_SDP_SENDONLY;
  policy.audio_direction = AULA_SDP_SENDRECV;
  policy.allow_pcmu = 1;
  policy.allow_pcma = 1;
  policy.allow_telephone_event = 1;
  policy.allow_h264_packetization_mode_1 = 1;
  policy.remote_target_authorizer = accept_authorizer;
  return policy;
}

static void test_answer_negotiates_codecs_and_direction(void) {
  aula_sdp_session *offer = parse_offer_text(valid_offer);
  aula_sdp_session *answer = NULL;
  aula_sdp_negotiated_session *negotiated = NULL;
  aula_sdp_answer_policy policy = make_policy();
  const aula_sdp_negotiated_media *video;
  const aula_sdp_negotiated_media *audio;
  AULA_CHECK(offer != NULL);
  if (offer == NULL) return;
  AULA_CHECK_STATUS(aula_sdp_answer_remote_offer(offer, &policy, &answer,
                    &negotiated), AULA_STATUS_OK);
  AULA_CHECK(answer != NULL && negotiated != NULL);
  if (answer == NULL || negotiated == NULL) goto cleanup;
  video = aula_sdp_negotiated_get_video(negotiated);
  audio = aula_sdp_negotiated_get_audio(negotiated);
  AULA_CHECK(video != NULL && audio != NULL);
  if (video != NULL) {
    AULA_CHECK(strcmp(video->remote_rtp_address, "192.168.1.10") == 0);
    AULA_CHECK(video->remote_rtp_port == 40000U);
    AULA_CHECK(video->remote_rtcp_port == 40001U);
    AULA_CHECK(video->local_rtp_port == 50000U);
    AULA_CHECK(video->local_rtcp_port == 50001U);
    AULA_CHECK(video->local_direction == AULA_SDP_SENDONLY);
    AULA_CHECK(video->remote_direction == AULA_SDP_SENDRECV);
    AULA_CHECK(video->srtp_enabled == 0);
    AULA_CHECK(video->codec_count == 1U);
    if (video->codec_count == 1U) {
      AULA_CHECK(video->codecs[0].kind == AULA_SDP_CODEC_H264);
      AULA_CHECK(video->codecs[0].payload_type == 96U);
    }
  }
  if (audio != NULL) {
    AULA_CHECK(audio->remote_rtp_port == 40002U);
    AULA_CHECK(audio->local_rtp_port == 50002U);
    AULA_CHECK(audio->local_direction == AULA_SDP_SENDRECV);
    AULA_CHECK(audio->codec_count >= 1U);
    if (audio->codec_count >= 1U) {
      AULA_CHECK(audio->codecs[0].kind == AULA_SDP_CODEC_PCMU);
      AULA_CHECK(audio->codecs[0].payload_type == 0U);
      AULA_CHECK(audio->codecs[0].clock_rate == 8000U);
    }
  }
  /* The answer itself is a well-formed SDP with the local ports. */
  if (aula_sdp_get_video(answer) != NULL) {
    AULA_CHECK(aula_sdp_get_video(answer)->rtp_port == 50000U);
    AULA_CHECK(aula_sdp_get_video(answer)->direction == AULA_SDP_SENDONLY);
  }
  if (aula_sdp_get_audio(answer) != NULL)
    AULA_CHECK(aula_sdp_get_audio(answer)->rtp_port == 50002U);
cleanup:
  aula_sdp_negotiated_session_destroy(negotiated);
  aula_sdp_session_destroy(answer);
  aula_sdp_session_destroy(offer);
}

static void test_answer_honours_codec_policy(void) {
  aula_sdp_session *offer = parse_offer_text(
      "v=0\r\no=- 1 1 IN IP4 192.168.1.10\r\ns=-\r\n"
      "c=IN IP4 192.168.1.10\r\nt=0 0\r\n"
      "m=video 40000 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 " SDP_VIDEO_FMTP "\r\n"
      "m=audio 40002 RTP/AVP 8 0\r\n"
      "a=rtpmap:8 PCMA/8000\r\na=rtpmap:0 PCMU/8000\r\n");
  aula_sdp_session *answer = NULL;
  aula_sdp_negotiated_session *negotiated = NULL;
  aula_sdp_answer_policy policy = make_policy();
  const aula_sdp_negotiated_media *audio;
  AULA_CHECK(offer != NULL);
  if (offer == NULL) return;
  /* With PCMU forbidden the remaining G.711 law is selected. */
  policy.allow_pcmu = 0;
  AULA_CHECK_STATUS(aula_sdp_answer_remote_offer(offer, &policy, &answer,
                    &negotiated), AULA_STATUS_OK);
  audio = aula_sdp_negotiated_get_audio(negotiated);
  AULA_CHECK(audio != NULL && audio->codec_count >= 1U);
  if (audio != NULL && audio->codec_count >= 1U) {
    AULA_CHECK(audio->codecs[0].kind == AULA_SDP_CODEC_PCMA);
    AULA_CHECK(audio->codecs[0].payload_type == 8U);
  }
  aula_sdp_negotiated_session_destroy(negotiated);
  aula_sdp_session_destroy(answer);
  answer = NULL;
  negotiated = NULL;
  /* With no usable audio codec the offer cannot be answered. */
  policy.allow_pcma = 0;
  AULA_CHECK(aula_sdp_answer_remote_offer(offer, &policy, &answer,
             &negotiated) != AULA_STATUS_OK);
  AULA_CHECK(answer == NULL && negotiated == NULL);
  aula_sdp_session_destroy(offer);
}

static void test_answer_rejects_invalid_arguments(void) {
  aula_sdp_session *offer = parse_offer_text(valid_offer);
  aula_sdp_session *answer = NULL;
  aula_sdp_negotiated_session *negotiated = NULL;
  aula_sdp_answer_policy policy = make_policy();
  AULA_CHECK(offer != NULL);
  if (offer == NULL) return;
  AULA_CHECK_STATUS(aula_sdp_answer_remote_offer(NULL, &policy, &answer,
                    &negotiated), AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK_STATUS(aula_sdp_answer_remote_offer(offer, NULL, &answer,
                    &negotiated), AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK_STATUS(aula_sdp_answer_remote_offer(offer, &policy, NULL,
                    &negotiated), AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK(answer == NULL && negotiated == NULL);
  AULA_CHECK(aula_sdp_get_video(NULL) == NULL);
  AULA_CHECK(aula_sdp_get_audio(NULL) == NULL);
  AULA_CHECK(aula_sdp_negotiated_get_video(NULL) == NULL);
  AULA_CHECK(aula_sdp_negotiated_get_audio(NULL) == NULL);
  aula_sdp_session_destroy(offer);
}

int main(void) {
  test_parse_valid_offer();
  test_parse_accepts_lf_and_session_direction();
  test_parse_rejects_malformed_input();
  test_parse_rejects_bad_media();
  test_serialize_round_trip();
  test_answer_negotiates_codecs_and_direction();
  test_answer_honours_codec_policy();
  test_answer_rejects_invalid_arguments();
  return aula_test_result("test_sdp");
}
