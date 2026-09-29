#include "request_decode.h"

#include <ctype.h>
#include <string.h>

#define CONTROL_JSON_MAX_MEMBERS 6U
#define CONTROL_JSON_MAX_KEY 32U
#define CONTROL_JSON_MAX_VALUE 256U
#define CONTROL_MEDIA_JSON_MAX_BYTES 48U
#define CONTROL_VIDEO_MEDIA_JSON_MAX_BYTES 32U

typedef struct control_json_member {
  char key[CONTROL_JSON_MAX_KEY];
  char value[CONTROL_JSON_MAX_VALUE + 1U];
  int value_is_string;
} control_json_member;

typedef struct control_json_object {
  control_json_member members[CONTROL_JSON_MAX_MEMBERS];
  size_t count;
} control_json_object;

typedef struct control_json_cursor {
  const uint8_t *data;
  size_t length;
  size_t offset;
} control_json_cursor;

static void control_json_wipe(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) {
    *cursor++ = 0U;
    --length;
  }
}

static void skip_space(control_json_cursor *cursor) {
  while (cursor->offset < cursor->length &&
         isspace((unsigned char)cursor->data[cursor->offset]))
    ++cursor->offset;
}

static int consume(control_json_cursor *cursor, uint8_t expected) {
  skip_space(cursor);
  if (cursor->offset >= cursor->length ||
      cursor->data[cursor->offset] != expected) return 0;
  ++cursor->offset;
  return 1;
}

static int parse_string(control_json_cursor *cursor, char *output,
                        size_t capacity) {
  size_t used = 0U;
  if (!consume(cursor, (uint8_t)'\"')) return 0;
  while (cursor->offset < cursor->length) {
    uint8_t byte = cursor->data[cursor->offset++];
    if (byte == (uint8_t)'\"') {
      if (used >= capacity) return 0;
      output[used] = '\0';
      return 1;
    }
    /* The accepted schemas are ASCII identifiers, digits, and enum tokens.
     * Reject escapes instead of growing this trust-boundary parser into a
     * general JSON or Unicode implementation. */
    if (byte == (uint8_t)'\\' || byte < 0x20U || byte > 0x7eU ||
        used + 1U >= capacity) return 0;
    output[used++] = (char)byte;
  }
  return 0;
}

static int object_has_key(const control_json_object *object, const char *key) {
  size_t index;
  for (index = 0U; index < object->count; ++index)
    if (strcmp(object->members[index].key, key) == 0) return 1;
  return 0;
}

static int parse_member(control_json_cursor *cursor, control_json_object *object) {
  control_json_member *member;
  if (object->count == CONTROL_JSON_MAX_MEMBERS) return 0;
  member = &object->members[object->count];
  if (!parse_string(cursor, member->key, sizeof(member->key)) ||
      object_has_key(object, member->key) || !consume(cursor, (uint8_t)':')) return 0;
  skip_space(cursor);
  if (cursor->offset < cursor->length && cursor->data[cursor->offset] == (uint8_t)'"') {
    if (!parse_string(cursor, member->value, sizeof(member->value))) return 0;
    member->value_is_string = 1;
  } else {
    size_t used = 0U;
    while (cursor->offset < cursor->length &&
           isdigit((unsigned char)cursor->data[cursor->offset])) {
      if (used + 1U >= sizeof(member->value)) return 0;
      member->value[used++] = (char)cursor->data[cursor->offset++];
    }
    if (used == 0U) return 0;
    member->value[used] = '\0';
  }
  ++object->count;
  return 1;
}

static int object_is_complete(control_json_cursor *cursor) {
  skip_space(cursor);
  if (cursor->offset >= cursor->length ||
      cursor->data[cursor->offset] != (uint8_t)'}') return 0;
  ++cursor->offset;
  skip_space(cursor);
  return cursor->offset == cursor->length;
}

static int parse_object(aula_bytes payload, control_json_object *object) {
  control_json_cursor cursor;
  if (payload.data == NULL || payload.length == 0U || object == NULL) return 0;
  (void)memset(object, 0, sizeof(*object));
  cursor.data = payload.data;
  cursor.length = payload.length;
  cursor.offset = 0U;
  if (!consume(&cursor, (uint8_t)'{')) return 0;
  if (object_is_complete(&cursor)) return 1;
  for (;;) {
    if (!parse_member(&cursor, object)) return 0;
    if (object_is_complete(&cursor)) return 1;
    if (!consume(&cursor, (uint8_t)',')) return 0;
  }
}

static const char *member_value(const control_json_object *object,
                                const char *key) {
  size_t index;
  for (index = 0U; index < object->count; ++index)
    if (strcmp(object->members[index].key, key) == 0)
      return object->members[index].value;
  return NULL;
}

static int member_is_string(const control_json_object *object, const char *key) {
  size_t index;
  for (index = 0U; index < object->count; ++index)
    if (strcmp(object->members[index].key, key) == 0)
      return object->members[index].value_is_string;
  return 0;
}

static int all_values_are_strings(const control_json_object *object) {
  size_t index;
  for (index = 0U; index < object->count; ++index)
    if (object->members[index].value_is_string == 0) return 0;
  return 1;
}

static int copy_optional(char *output, size_t capacity, const char *value,
                         const char **bound) {
  size_t length;
  if (output == NULL || capacity == 0U || value == NULL || bound == NULL)
    return 0;
  length = strlen(value);
  if (length >= capacity) return 0;
  (void)memcpy(output, value, length + 1U);
  *bound = length == 0U ? NULL : output;
  return 1;
}

static int decode_profile(const char *profile, aula_zoom_profile *out_profile) {
  if (strcmp(profile, "zoom_direct") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_DIRECT_CRC;
  } else if (strcmp(profile, "zoom_proxy") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_PROXY_REGISTRATION;
  } else if (strcmp(profile, "private_lab") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_PRIVATE_LAB;
  } else {
    return 0;
  }
  return 1;
}

static int decode_layout(const char *layout, aula_zoom_layout *out_layout) {
  if (strcmp(layout, "gallery") == 0) {
    *out_layout = AULA_ZOOM_LAYOUT_GALLERY;
  } else if (strcmp(layout, "full_screen") == 0) {
    *out_layout = AULA_ZOOM_LAYOUT_FULL_SCREEN;
  } else if (strcmp(layout, "dual_video") == 0) {
    *out_layout = AULA_ZOOM_LAYOUT_DUAL_VIDEO;
  } else {
    return 0;
  }
  return 1;
}

aula_status aula_control_decode_originate(
    aula_bytes payload, aula_control_originate_request *out_request) {
  control_json_object object;
  const char *profile;
  const char *layout;
  const char *meeting;
  aula_status status = AULA_STATUS_INVALID_DATA;
  if (out_request == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_request, 0, sizeof(*out_request));
  if (!parse_object(payload, &object) || object.count != 6U ||
      !all_values_are_strings(&object))
    goto cleanup;
  profile = member_value(&object, "profile");
  layout = member_value(&object, "layout");
  meeting = member_value(&object, "meeting_id");
  if (profile == NULL || layout == NULL || meeting == NULL ||
      strlen(meeting) >= sizeof(out_request->meeting_id))
    goto cleanup;
  (void)memcpy(out_request->meeting_id, meeting, strlen(meeting) + 1U);
  out_request->dial.meeting_id = out_request->meeting_id;
  if (!decode_profile(profile, &out_request->dial.profile) ||
      !decode_layout(layout, &out_request->dial.layout)) goto cleanup;
  if (!copy_optional(out_request->passcode, sizeof(out_request->passcode),
                     member_value(&object, "passcode"), &out_request->dial.passcode) ||
      !copy_optional(out_request->host_key, sizeof(out_request->host_key),
                     member_value(&object, "host_key"), &out_request->dial.host_key) ||
      !copy_optional(out_request->dial_code, sizeof(out_request->dial_code),
                     member_value(&object, "dial_code"), &out_request->dial.dial_code))
    goto cleanup;
  status = AULA_STATUS_OK;
cleanup:
  control_json_wipe(&object, sizeof(object));
  if (status != AULA_STATUS_OK)
    control_json_wipe(out_request, sizeof(*out_request));
  return status;
}

aula_status aula_control_decode_dtmf(aula_bytes payload,
                                       uint8_t *out_digit) {
  static const char symbols[] = "0123456789*#ABCD";
  control_json_object object;
  const char *tone;
  const char *match;
  if (out_digit == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (!parse_object(payload, &object) || object.count != 1U ||
      !all_values_are_strings(&object) ||
      (tone = member_value(&object, "tone")) == NULL || tone[0] == '\0' ||
      tone[1] != '\0' || (match = strchr(symbols, tone[0])) == NULL)
    return AULA_STATUS_INVALID_DATA;
  *out_digit = (uint8_t)(match - symbols);
  return AULA_STATUS_OK;
}

aula_status aula_control_decode_credentials(
    aula_bytes payload, aula_control_credentials_request *out_request) {
  control_json_object object;
  const char *username;
  const char *password;
  size_t username_length;
  size_t password_length;
  aula_status status = AULA_STATUS_INVALID_DATA;
  if (out_request == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_request, 0, sizeof(*out_request));
  if (!parse_object(payload, &object) || object.count != 2U ||
      !all_values_are_strings(&object) ||
      (username = member_value(&object, "username")) == NULL ||
      (password = member_value(&object, "password")) == NULL)
    goto cleanup;
  username_length = strlen(username);
  password_length = strlen(password);
  if (username_length == 0U || username_length >= sizeof(out_request->username) ||
      password_length < 12U || password_length >= sizeof(out_request->password))
    goto cleanup;
  (void)memcpy(out_request->username, username, username_length + 1U);
  (void)memcpy(out_request->password, password, password_length + 1U);
  status = AULA_STATUS_OK;
cleanup:
  control_json_wipe(&object, sizeof(object));
  if (status != AULA_STATUS_OK)
    control_json_wipe(out_request, sizeof(*out_request));
  return status;
}

static aula_status decode_video_media_request(
    aula_bytes payload, const control_json_object *object,
    aula_control_media_request *out_request) {
  const char *mode;
  if (payload.length > CONTROL_VIDEO_MEDIA_JSON_MAX_BYTES || object->count != 1U ||
      (mode = member_value(object, "mode")) == NULL) return AULA_STATUS_INVALID_DATA;
  if (strcmp(mode, "enabled") == 0) {
    out_request->video_transmit_enabled = 1;
  } else if (strcmp(mode, "disabled") != 0) {
    return AULA_STATUS_INVALID_DATA;
  }
  out_request->action = AULA_CONTROL_MEDIA_VIDEO_TRANSMIT;
  return AULA_STATUS_OK;
}

static aula_status decode_media_action_request(
    const control_json_object *object, aula_control_media_request *out_request) {
  const char *action = member_value(object, "action");
  const char *value = member_value(object, "value");
  if (object->count != 2U || action == NULL || value == NULL ||
      member_value(object, "mode") != NULL) return AULA_STATUS_INVALID_DATA;
  if (strcmp(action, "audio_mute") == 0) {
    if (strcmp(value, "enabled") == 0) out_request->audio_muted = 1;
    else if (strcmp(value, "disabled") == 0) out_request->audio_muted = 0;
    else return AULA_STATUS_INVALID_DATA;
    out_request->action = AULA_CONTROL_MEDIA_AUDIO_MUTE;
    return AULA_STATUS_OK;
  }
  if (strcmp(action, "keyframe") == 0 && strcmp(value, "request") == 0) {
    out_request->action = AULA_CONTROL_MEDIA_KEYFRAME;
    return AULA_STATUS_OK;
  }
  if (strcmp(action, "layout_next") == 0 && strcmp(value, "request") == 0) {
    out_request->action = AULA_CONTROL_MEDIA_LAYOUT_NEXT;
    return AULA_STATUS_OK;
  }
  return AULA_STATUS_INVALID_DATA;
}

aula_status aula_control_decode_media(
    aula_bytes payload, aula_control_media_request *out_request) {
  control_json_object object;
  if (out_request == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_request, 0, sizeof(*out_request));
  if (payload.length > CONTROL_MEDIA_JSON_MAX_BYTES || !parse_object(payload, &object) ||
      !all_values_are_strings(&object)) {
    return AULA_STATUS_INVALID_DATA;
  }
  return member_value(&object, "mode") != NULL
      ? decode_video_media_request(payload, &object, out_request)
      : decode_media_action_request(&object, out_request);
}

static int decode_revision(const char *value, uint32_t *out_revision) {
  uint64_t parsed = 0U;
  size_t index;
  if (value == NULL || value[0] == '\0' || out_revision == NULL) return 0;
  for (index = 0U; value[index] != '\0'; ++index) {
    if (!isdigit((unsigned char)value[index])) return 0;
    parsed = parsed * UINT64_C(10) + (uint64_t)(value[index] - '0');
    if (parsed > UINT32_MAX) return 0;
  }
  if (parsed == 0U) return 0;
  *out_revision = (uint32_t)parsed;
  return 1;
}

static int decode_settings_members(const control_json_object *object,
                                   const char **profile, const char **media,
                                   const char **tls, uint32_t *revision) {
  const char *revision_value;
  if ((object->count != 3U && object->count != 4U) ||
      (*profile = member_value(object, "profile")) == NULL ||
      (*media = member_value(object, "media")) == NULL ||
      !member_is_string(object, "profile") ||
      !member_is_string(object, "media") ||
      member_is_string(object, "revision")) return 0;
  *tls = member_value(object, "tls");
  if ((object->count == 4U && *tls == NULL) ||
      (*tls != NULL && !member_is_string(object, "tls"))) return 0;
  revision_value = member_value(object, "revision");
  return decode_revision(revision_value, revision);
}

static int decode_settings_media(const char *value, int *out_managed) {
  if (strcmp(value, "managed") == 0) {
    *out_managed = 1;
    return 1;
  }
  if (strcmp(value, "disabled") == 0) {
    *out_managed = 0;
    return 1;
  }
  return 0;
}

static int decode_settings_tls(const char *value) {
  return value == NULL || strcmp(value, "verified") == 0 ||
         strcmp(value, "required") == 0 ||
         strcmp(value, "not_required") == 0;
}

aula_status aula_control_decode_settings(
    aula_bytes payload, aula_control_settings_request *out_request) {
  control_json_object object;
  const char *profile;
  const char *media;
  const char *tls;
  if (out_request == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_request, 0, sizeof(*out_request));
  if (!parse_object(payload, &object) ||
      !decode_settings_members(&object, &profile, &media, &tls,
                               &out_request->revision) ||
      !decode_profile(profile, &out_request->profile) ||
      !decode_settings_media(media, &out_request->media_managed) ||
      !decode_settings_tls(tls))
    return AULA_STATUS_INVALID_DATA;
  return AULA_STATUS_OK;
}

int aula_control_payload_is_empty_object(aula_bytes payload) {
  control_json_object object;
  return payload.length == 0U || (parse_object(payload, &object) && object.count == 0U);
}
