#ifndef LS200_SIPD_CONTROL_REQUEST_DECODE_H
#define LS200_SIPD_CONTROL_REQUEST_DECODE_H

#include "ls200_sipd/control_protocol.h"
#include "ls200_sipd/zoom.h"

typedef struct ls200_control_originate_request {
  ls200_zoom_dial_request dial;
  char meeting_id[LS200_ZOOM_MAX_MEETING_ID_BYTES];
  char passcode[LS200_ZOOM_MAX_PASSCODE_BYTES];
  char host_key[LS200_ZOOM_MAX_HOST_KEY_BYTES];
  char dial_code[LS200_ZOOM_MAX_DIAL_CODE_BYTES];
} ls200_control_originate_request;

typedef struct ls200_control_credentials_request {
  char username[257];
  char password[257];
} ls200_control_credentials_request;

typedef enum ls200_control_media_action {
  LS200_CONTROL_MEDIA_VIDEO_TRANSMIT = 0,
  LS200_CONTROL_MEDIA_AUDIO_MUTE,
  LS200_CONTROL_MEDIA_KEYFRAME,
  LS200_CONTROL_MEDIA_LAYOUT_NEXT
} ls200_control_media_action;

typedef struct ls200_control_media_request {
  ls200_control_media_action action;
  int video_transmit_enabled;
  int audio_muted;
} ls200_control_media_request;

/* Settings are deliberately small safe enums.  They never contain addresses,
 * URIs, file paths, environment values, or credential material. */
typedef struct ls200_control_settings_request {
  uint32_t revision;
  ls200_zoom_profile profile;
  int media_managed;
} ls200_control_settings_request;

ls200_status ls200_control_decode_originate(
    ls200_bytes payload, ls200_control_originate_request *out_request);
ls200_status ls200_control_decode_dtmf(ls200_bytes payload,
                                       uint8_t *out_digit);
ls200_status ls200_control_decode_credentials(
    ls200_bytes payload, ls200_control_credentials_request *out_request);
ls200_status ls200_control_decode_media(
    ls200_bytes payload, ls200_control_media_request *out_request);
ls200_status ls200_control_decode_settings(
    ls200_bytes payload, ls200_control_settings_request *out_request);
int ls200_control_payload_is_empty_object(ls200_bytes payload);

#endif
