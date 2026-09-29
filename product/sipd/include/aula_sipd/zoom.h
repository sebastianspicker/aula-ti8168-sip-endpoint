#ifndef AULA_SIPD_ZOOM_H
#define AULA_SIPD_ZOOM_H

#include "aula_sipd/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This model is deliberately typed. Callers do not hand an HTTP URL or a
 * pre-composed SIP URI to the direct CRC profile. The resulting bytes are
 * sensitive operational input and must never be logged. */
#define AULA_ZOOM_CRC_DOMAIN "zoomcrc.com"
#define AULA_ZOOM_MAX_MEETING_ID_BYTES 17U
#define AULA_ZOOM_MAX_PASSCODE_BYTES 33U
#define AULA_ZOOM_MAX_HOST_KEY_BYTES 17U
#define AULA_ZOOM_MAX_DIAL_CODE_BYTES 65U
#define AULA_ZOOM_MAX_TARGET_URI_BYTES 512U

typedef enum aula_zoom_profile {
  AULA_ZOOM_PROFILE_DIRECT_CRC = 0,
  AULA_ZOOM_PROFILE_PROXY_REGISTRATION,
  AULA_ZOOM_PROFILE_PRIVATE_LAB
} aula_zoom_profile;

typedef enum aula_zoom_layout {
  AULA_ZOOM_LAYOUT_GALLERY = 0,
  AULA_ZOOM_LAYOUT_FULL_SCREEN,
  AULA_ZOOM_LAYOUT_DUAL_VIDEO
} aula_zoom_layout;

typedef struct aula_zoom_dial_request {
  aula_zoom_profile profile;
  const char *meeting_id;
  const char *passcode; /* optional */
  aula_zoom_layout layout;
  const char *host_key; /* optional */
  /* Optional opaque Zoom-issued dial or participant identifier. Documented
   * feature codes often start with 1 or 2, but API-issued identifiers are not
   * prefix-constrained. */
  const char *dial_code;
  /* Direct CRC accepts only NULL or the official fixed domain. Proxy and
   * private-lab profiles require an approved SIP domain or numeric IPv4 host. */
  const char *target_domain;
  /* This endpoint is copied from the administrator-approved profile. It is
   * never decoded from an HTTP or LSZ1 originate payload. */
  uint16_t target_port;
  int has_target_port;
} aula_zoom_dial_request;

typedef struct aula_zoom_dial_target {
  aula_zoom_profile profile;
  int requires_proxy_registration;
  aula_transport transport;
  uint16_t port;
  int has_explicit_port;
} aula_zoom_dial_target;

/* Writes a SIP URI built from the documented positional Zoom SIP dial fields:
 * meeting.passcode.command.host-key.reserved.dial-code@domain. The caller owns
 * and must wipe the output buffer after it has been handed to the SIP adapter. */
aula_status aula_zoom_build_dial_target(
    const aula_zoom_dial_request *request, aula_mutable_bytes *output);

/* Parses a built target without retaining or disclosing its secret fields. */
aula_status aula_zoom_classify_dial_target(
    const char *target_uri, aula_zoom_dial_target *out_target);

/* Emits profile metadata only. It never emits a meeting ID, passcode, host key,
 * dial code, endpoint, or URI. */
aula_status aula_zoom_format_redacted_profile(
    aula_zoom_profile profile, aula_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
