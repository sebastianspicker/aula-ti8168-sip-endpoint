#include "gateway_internal.h"

#include <string.h>

static int json_nonnegative_integer(json_t *object, const char *key) {
  json_t *value = json_object_get(object, key);
  return json_is_integer(value) && json_integer_value(value) >= 0;
}

static int aec_state_known(const char *state) {
  static const char *const states[] = {
      "inactive", "available", "uncalibrated", "calibrated"};
  size_t index;
  if (state == NULL) return 0;
  for (index = 0U; index < sizeof(states) / sizeof(states[0]); ++index) {
    if (strcmp(state, states[index]) == 0) return 1;
  }
  return 0;
}

static int aec_state_consistent(const char *state, int available, int active) {
  int configured = strcmp(state, "uncalibrated") == 0 || strcmp(state, "calibrated") == 0;
  if (active != 0 && available == 0) return 0;
  if (available == 0 && strcmp(state, "inactive") != 0) return 0;
  if (configured != 0 && available == 0) return 0;
  if (configured == 0 && active != 0) return 0;
  return 1;
}

int gateway_aec_status_valid(json_t *aec) {
  static const char *const keys[] = {
      "delay_state", "available", "active", "processed_frames",
      "reference_underflows", "resets"};
  const char *state;
  int available;
  int active;
  if (aec == NULL) return 0;
  if (!gateway_json_object_exact(aec, keys, 6U)) return 0;
  state = json_string_value(json_object_get(aec, "delay_state"));
  available = json_is_true(json_object_get(aec, "available"));
  active = json_is_true(json_object_get(aec, "active"));
  if (!aec_state_known(state) ||
      !json_is_boolean(json_object_get(aec, "available")) ||
      !json_is_boolean(json_object_get(aec, "active"))) return 0;
  return aec_state_consistent(state, available, active) &&
      json_nonnegative_integer(aec, "processed_frames") &&
      json_nonnegative_integer(aec, "reference_underflows") &&
      json_nonnegative_integer(aec, "resets");
}
