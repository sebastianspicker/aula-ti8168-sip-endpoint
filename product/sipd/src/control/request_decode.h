#ifndef AULA_SIPD_CONTROL_REQUEST_DECODE_H
#define AULA_SIPD_CONTROL_REQUEST_DECODE_H

#include "aula_sipd/control_protocol.h"
#include "aula_sipd/zoom.h"

typedef struct aula_control_originate_request {
  aula_zoom_dial_request dial;
  char meeting_id[AULA_ZOOM_MAX_MEETING_ID_BYTES];
  char passcode[AULA_ZOOM_MAX_PASSCODE_BYTES];
  char host_key[AULA_ZOOM_MAX_HOST_KEY_BYTES];
  char dial_code[AULA_ZOOM_MAX_DIAL_CODE_BYTES];
} aula_control_originate_request;

typedef struct aula_control_credentials_request {
  char username[257];
  char password[257];
} aula_control_credentials_request;

typedef enum aula_control_media_action {
  AULA_CONTROL_MEDIA_VIDEO_TRANSMIT = 0,
  AULA_CONTROL_MEDIA_AUDIO_MUTE,
  AULA_CONTROL_MEDIA_KEYFRAME,
  AULA_CONTROL_MEDIA_LAYOUT_NEXT
} aula_control_media_action;

typedef struct aula_control_media_request {
  aula_control_media_action action;
  int video_transmit_enabled;
  int audio_muted;
} aula_control_media_request;

/* Settings are deliberately small safe enums.  They never contain addresses,
 * URIs, file paths, environment values, or credential material. */
typedef struct aula_control_settings_request {
  uint32_t revision;
  aula_zoom_profile profile;
  int media_managed;
} aula_control_settings_request;

aula_status aula_control_decode_originate(
    aula_bytes payload, aula_control_originate_request *out_request);
aula_status aula_control_decode_dtmf(aula_bytes payload,
                                       uint8_t *out_digit);
aula_status aula_control_decode_credentials(
    aula_bytes payload, aula_control_credentials_request *out_request);
aula_status aula_control_decode_media(
    aula_bytes payload, aula_control_media_request *out_request);
aula_status aula_control_decode_settings(
    aula_bytes payload, aula_control_settings_request *out_request);
int aula_control_payload_is_empty_object(aula_bytes payload);

#endif
