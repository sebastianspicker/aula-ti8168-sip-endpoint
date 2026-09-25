#ifndef LS200_SIPD_ZOOM_H
#define LS200_SIPD_ZOOM_H

#include "ls200_sipd/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This model is deliberately typed. Callers do not hand an HTTP URL or a
 * pre-composed SIP URI to the direct CRC profile. The resulting bytes are
 * sensitive operational input and must never be logged. */
#define LS200_ZOOM_CRC_DOMAIN "zoomcrc.com"
#define LS200_ZOOM_MAX_MEETING_ID_BYTES 17U
#define LS200_ZOOM_MAX_PASSCODE_BYTES 33U
#define LS200_ZOOM_MAX_HOST_KEY_BYTES 17U
#define LS200_ZOOM_MAX_DIAL_CODE_BYTES 65U
#define LS200_ZOOM_MAX_TARGET_URI_BYTES 512U

typedef enum ls200_zoom_profile {
  LS200_ZOOM_PROFILE_DIRECT_CRC = 0,
  LS200_ZOOM_PROFILE_PROXY_REGISTRATION,
  LS200_ZOOM_PROFILE_PRIVATE_LAB
} ls200_zoom_profile;

typedef enum ls200_zoom_layout {
  LS200_ZOOM_LAYOUT_GALLERY = 0,
  LS200_ZOOM_LAYOUT_FULL_SCREEN,
  LS200_ZOOM_LAYOUT_DUAL_VIDEO
} ls200_zoom_layout;

typedef struct ls200_zoom_dial_request {
  ls200_zoom_profile profile;
  const char *meeting_id;
  const char *passcode; /* optional */
  ls200_zoom_layout layout;
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
} ls200_zoom_dial_request;

typedef struct ls200_zoom_dial_target {
  ls200_zoom_profile profile;
  int requires_proxy_registration;
  ls200_transport transport;
  uint16_t port;
  int has_explicit_port;
} ls200_zoom_dial_target;

/* Writes a SIP URI built from the documented positional Zoom SIP dial fields:
 * meeting.passcode.command.host-key.reserved.dial-code@domain. The caller owns
 * and must wipe the output buffer after it has been handed to the SIP adapter. */
ls200_status ls200_zoom_build_dial_target(
    const ls200_zoom_dial_request *request, ls200_mutable_bytes *output);

/* Parses a built target without retaining or disclosing its secret fields. */
ls200_status ls200_zoom_classify_dial_target(
    const char *target_uri, ls200_zoom_dial_target *out_target);

/* Emits profile metadata only. It never emits a meeting ID, passcode, host key,
 * dial code, endpoint, or URI. */
ls200_status ls200_zoom_format_redacted_profile(
    ls200_zoom_profile profile, ls200_mutable_bytes *output);

#ifdef __cplusplus
}
#endif

#endif
