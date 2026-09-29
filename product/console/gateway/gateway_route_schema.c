#include "gateway_internal.h"

#include "../device/credentials.h"

#include <ctype.h>
#include <string.h>

static int safe_control_text(const char *value, size_t minimum, size_t maximum) {
  size_t index, length;
  if (value == NULL) return 0;
  length = strlen(value);
  if (length < minimum || length > maximum || strchr(value, '/') != NULL) return 0;
  for (index = 0U; index < length; ++index) {
    if ((unsigned char)value[index] < 0x21U || (unsigned char)value[index] > 0x7eU ||
        (index + 3U < length && tolower((unsigned char)value[index]) == 's' &&
         tolower((unsigned char)value[index + 1U]) == 'i' &&
         tolower((unsigned char)value[index + 2U]) == 'p' && value[index + 3U] == ':')) return 0;
  }
  return 1;
}

int gateway_digits_only(const char *value, int optional) {
  size_t index;
  if (value == NULL || value[0] == '\0') return optional;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isdigit((unsigned char)value[index])) return 0;
  return 1;
}

static int dial_code_valid(const char *value) {
  size_t index;
  if (value == NULL || value[0] == '\0') return 1;
  for (index = 0U; value[index] != '\0'; ++index)
    if (!isalnum((unsigned char)value[index])) return 0;
  return 1;
}

int gateway_schema_call_request(const char *body) {
  static const char *const keys[] = {"dial_code", "host_key", "layout", "meeting_id", "passcode", "profile"};
  static const char *const layouts[] = {"gallery", "full_screen", "dual_video"};
  static const char *const profiles[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  json_error_t error;
  json_t *root = gateway_parse_json(body, &error);
  const char *meeting, *profile, *passcode, *host_key, *dial_code;
  int valid;
  if (root == NULL || !gateway_json_object_exact(root, keys, 6U)) {
    if (root != NULL) json_decref(root);
    return 0;
  }
  meeting = json_string_value(json_object_get(root, "meeting_id"));
  profile = json_string_value(json_object_get(root, "profile"));
  passcode = json_string_value(json_object_get(root, "passcode"));
  host_key = json_string_value(json_object_get(root, "host_key"));
  dial_code = json_string_value(json_object_get(root, "dial_code"));
  valid = safe_control_text(meeting, 9U, 11U) && gateway_digits_only(meeting, 0) &&
      safe_control_text(profile, 1U, 32U) && gateway_schema_enum(body, "profile", profiles, 3U) &&
      safe_control_text(passcode, 0U, 64U) && gateway_digits_only(passcode, 1) &&
      safe_control_text(host_key, 0U, 10U) && gateway_digits_only(host_key, 1) &&
      safe_control_text(dial_code, 0U, 64U) && dial_code_valid(dial_code);
  if (valid) valid = gateway_schema_enum(body, "layout", layouts, 3U);
  json_decref(root);
  return valid;
}

int gateway_schema_settings_request(const char *body) {
  static const char *const keys[] = {"media", "profile", "revision"};
  static const char *const legacy_keys[] = {"media", "profile", "revision", "tls"};
  static const char *const media[] = {"managed", "disabled"};
  static const char *const profiles[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  static const char *const tls[] = {"required", "verified", "not_required"};
  json_error_t error;
  json_t *root = gateway_parse_json(body, &error);
  const char *profile;
  int has_tls, valid;
  if (root == NULL) return 0;
  has_tls = json_object_get(root, "tls") != NULL;
  if (!(has_tls ? gateway_json_object_exact(root, legacy_keys, 4U) :
      gateway_json_object_exact(root, keys, 3U))) {
    json_decref(root);
    return 0;
  }
  profile = json_string_value(json_object_get(root, "profile"));
  valid = json_is_integer(json_object_get(root, "revision")) &&
      json_integer_value(json_object_get(root, "revision")) >= 1 &&
      (uint64_t)json_integer_value(json_object_get(root, "revision")) <= UINT_MAX &&
      safe_control_text(profile, 1U, 32U) && gateway_schema_enum(body, "profile", profiles, 3U) &&
      gateway_schema_enum(body, "media", media, 2U) &&
      (!has_tls || gateway_schema_enum(body, "tls", tls, 3U));
  json_decref(root);
  return valid;
}

int gateway_schema_dtmf_request(const char *body) {
  static const char *const k[] = {"tone"};
  static const char *const t[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#", "A", "B", "C", "D"};
  return gateway_schema_keys(body, k, 1U) && gateway_schema_enum(body, "tone", t, 16U);
}

int gateway_schema_media_request(const char *body) {
  static const char *const legacy_keys[] = {"mode"};
  static const char *const modes[] = {"enabled", "disabled"};
  static const char *const action_keys[] = {"action", "value"};
  json_error_t error;
  json_t *root;
  const char *action, *value;
  int valid = 0;
  if (gateway_schema_keys(body, legacy_keys, 1U)) return gateway_schema_enum(body, "mode", modes, 2U);
  root = gateway_parse_json(body, &error);
  if (root == NULL || !gateway_json_object_exact(root, action_keys, 2U)) {
    if (root != NULL) json_decref(root);
    return 0;
  }
  action = json_string_value(json_object_get(root, "action"));
  value = json_string_value(json_object_get(root, "value"));
  if (action != NULL && value != NULL) {
    valid = (strcmp(action, "audio_mute") == 0 &&
        (strcmp(value, "enabled") == 0 || strcmp(value, "disabled") == 0)) ||
        ((strcmp(action, "keyframe") == 0 || strcmp(action, "layout_next") == 0) &&
         strcmp(value, "request") == 0);
  }
  json_decref(root);
  return valid;
}

int gateway_schema_credentials_request(const char *body) {
  static const char *const k[] = {"password", "username"};
  return gateway_schema_keys(body, k, 2U);
}

int gateway_schema_diagnostics_request(const char *body) {
  return body != NULL && strcmp(body, "{}") == 0;
}

int gateway_schema_oem_credentials(const char *body) {
  json_error_t error;
  json_t *arguments = gateway_parse_json(body, &error);
  int valid = aula_device_credentials_schema(arguments);
  json_decref(arguments);
  return valid;
}
