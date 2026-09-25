#include "sdp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ls200_sdp_fmtp_diagnostic {
  unsigned long profile;
  uint32_t packetization_mode;
  uint32_t level_asymmetry;
  uint32_t max_fs;
  uint32_t max_mbps;
  int profile_present;
  int packetization_present;
  int level_asymmetry_present;
  int max_fs_present;
  int max_mbps_present;
} ls200_sdp_fmtp_diagnostic;

static int ls200_sdp_diagnostic_profile(const char *value,
                                        unsigned long *out_profile) {
  char *end;
  unsigned long profile;
  if (value == NULL || strlen(value) != 6U) return 0;
  profile = strtoul(value, &end, 16);
  if (end != value + 6 || *end != '\0' || profile > 0xffffffUL) return 0;
  *out_profile = profile;
  return 1;
}

static void ls200_sdp_diagnostic_fmtp_field(
    char *field, ls200_sdp_fmtp_diagnostic *summary) {
  char *equals;
  while (*field == ' ') ++field;
  equals = strchr(field, '=');
  if (equals == NULL || equals == field || equals[1] == '\0') return;
  *equals++ = '\0';
  if (ls200_sdp_ascii_equal(field, "profile-level-id")) {
    summary->profile_present =
        ls200_sdp_diagnostic_profile(equals, &summary->profile);
  } else if (ls200_sdp_ascii_equal(field, "packetization-mode")) {
    summary->packetization_present =
        ls200_sdp_parse_u32(equals, &summary->packetization_mode);
  } else if (ls200_sdp_ascii_equal(field, "level-asymmetry-allowed")) {
    summary->level_asymmetry_present =
        ls200_sdp_parse_u32(equals, &summary->level_asymmetry);
  } else if (ls200_sdp_ascii_equal(field, "max-fs")) {
    summary->max_fs_present = ls200_sdp_parse_u32(equals, &summary->max_fs);
  } else if (ls200_sdp_ascii_equal(field, "max-mbps")) {
    summary->max_mbps_present =
        ls200_sdp_parse_u32(equals, &summary->max_mbps);
  }
}

static ls200_sdp_fmtp_diagnostic ls200_sdp_diagnostic_fmtp(
    const ls200_sdp_codec *codec) {
  ls200_sdp_fmtp_diagnostic summary;
  char copy[LS200_SDP_MAX_FMTP_BYTES];
  char *field;
  (void)memset(&summary, 0, sizeof(summary));
  if (codec->kind != LS200_SDP_CODEC_H264) return summary;
  if (codec->format_parameters == NULL || *codec->format_parameters == '\0' ||
      strlen(codec->format_parameters) >= sizeof(copy)) return summary;
  (void)memcpy(copy, codec->format_parameters,
               strlen(codec->format_parameters) + 1U);
  field = strtok(copy, ";");
  while (field != NULL) {
    ls200_sdp_diagnostic_fmtp_field(field, &summary);
    field = strtok(NULL, ";");
  }
  return summary;
}

static unsigned ls200_sdp_unmapped_payload_count(
    const ls200_sdp_stored_media *media) {
  unsigned count = 0U;
  unsigned payload;
  for (payload = 0U; payload < 128U; ++payload) {
    if (media->declared_payload_types[payload] != 0U &&
        ls200_sdp_find_codec_const(media, (uint8_t)payload) == NULL &&
        media->mapped_payload_types[payload] == 0U &&
        (media->is_video || payload != 9U)) ++count;
  }
  return count;
}

static void ls200_sdp_log_unmapped_payloads(
    const ls200_sdp_stored_media *media, int video) {
  unsigned payload;
  for (payload = 0U; payload < 128U; ++payload) {
    if (media->declared_payload_types[payload] != 0U &&
        ls200_sdp_find_codec_const(media, (uint8_t)payload) == NULL &&
        media->mapped_payload_types[payload] == 0U &&
        (media->is_video || payload != 9U))
      (void)fprintf(stderr,
          "ls200-sipd: sdp-stage=declared-unmapped video=%d payload=%u\n",
          video, payload);
  }
}

static void ls200_sdp_log_media_codecs(const ls200_sdp_stored_media *media,
                                       int remote, int video) {
  size_t index;
  for (index = 0U; index < media->value.codec_count; ++index) {
    const ls200_sdp_codec *codec = &media->codecs[index];
    ls200_sdp_fmtp_diagnostic fmtp = ls200_sdp_diagnostic_fmtp(codec);
    (void)fprintf(stderr,
        "ls200-sipd: sdp-stage=codec-summary remote=%d video=%d payload=%u codec=%d rate=%lu channels=%u h264_compatible=%d profile_present=%d profile=%lu packetization_present=%d packetization=%lu level_asymmetry_present=%d level_asymmetry=%lu max_fs_present=%d max_fs=%lu max_mbps_present=%d max_mbps=%lu rtp=%u rtcp=%u mux=%d srtp=%d\n",
        remote, video, (unsigned)codec->payload_type, (int)codec->kind,
        (unsigned long)codec->clock_rate, (unsigned)codec->channels,
        ls200_sdp_h264_fmtp_is_compatible(codec->format_parameters),
        fmtp.profile_present, fmtp.profile, fmtp.packetization_present,
        (unsigned long)fmtp.packetization_mode, fmtp.level_asymmetry_present,
        (unsigned long)fmtp.level_asymmetry, fmtp.max_fs_present,
        (unsigned long)fmtp.max_fs, fmtp.max_mbps_present,
        (unsigned long)fmtp.max_mbps, (unsigned)media->value.rtp_port,
        (unsigned)media->value.rtcp_port, media->value.rtcp_mux,
        media->srtp.present);
  }
}

static void ls200_sdp_log_media_diagnostics(
    const ls200_sdp_stored_media *media, int strict_offer, int remote,
    int video) {
  (void)fprintf(stderr,
      "ls200-sipd: sdp-stage=media-summary remote=%d video=%d reason=%d profile=%d direction=%d present=%d codecs=%lu unmapped=%u srtp=%d\n",
      remote, video,
      ls200_sdp_validation_reason(media, strict_offer, video),
      (int)media->value.profile, (int)media->value.direction, media->present,
      (unsigned long)media->value.codec_count,
      ls200_sdp_unmapped_payload_count(media), media->srtp.present);
  ls200_sdp_log_unmapped_payloads(media, video);
  ls200_sdp_log_media_codecs(media, remote, video);
}

void ls200_sdp_log_codec_diagnostics(const ls200_sdp_session *session,
                                     int strict_offer, int remote) {
  if (session == NULL) return;
  ls200_sdp_log_media_diagnostics(&session->video, strict_offer, remote, 1);
  ls200_sdp_log_media_diagnostics(&session->audio, strict_offer, remote, 0);
  (void)fflush(stderr);
}
