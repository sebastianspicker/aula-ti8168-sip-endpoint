#include "gateway_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int key_is_sensitive(const char *key) {
  static const char *const names[] = {"password", "secret", "token", "cookie", "authorization"};
  size_t index;
  if (key == NULL) return 1;
  for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index)
    if (strcasecmp(key, names[index]) == 0) return 1;
  return 0;
}

int gateway_json_value_is_safe(json_t *value) {
  const char *key;
  void *iterator;
  size_t index;
  if (json_is_null(value) || json_is_boolean(value) || json_is_integer(value) || json_is_real(value)) return 1;
  if (json_is_string(value)) return strlen(json_string_value(value)) <= 256U;
  if (json_is_array(value)) {
    if (json_array_size(value) > 32U) return 0;
    for (index = 0U; index < json_array_size(value); ++index)
      if (!gateway_json_value_is_safe(json_array_get(value, index))) return 0;
    return 1;
  }
  if (!json_is_object(value) || json_object_size(value) > 16U) return 0;
  iterator = json_object_iter(value);
  while (iterator != NULL) {
    key = json_object_iter_key(iterator);
    if (key_is_sensitive(key) || !gateway_json_value_is_safe(json_object_iter_value(iterator))) return 0;
    iterator = json_object_iter_next(value, iterator);
  }
  return 1;
}

static int diagnostics_state_valid(json_t *value, int ready_allowed) {
  const char *state = json_string_value(value);
  return state != NULL && (strcmp(state, "not_ready") == 0 || strcmp(state, "unavailable") == 0 ||
      (ready_allowed && strcmp(state, "ready") == 0));
}

static int diagnostics_check_valid(json_t *check, int ready_allowed) {
  static const char *const keys[] = {"state"};
  return gateway_json_object_exact(check, keys, 1U) &&
      diagnostics_state_valid(json_object_get(check, "state"), ready_allowed);
}

static int diagnostics_backend_valid(json_t *root) {
  static const char *const keys[] = {"state", "checks"};
  static const char *const check_keys[] = {
      "configuration", "sip_control", "media_readiness", "tls_profile_policy", "renderer_truth"};
  json_t *checks;
  if (!gateway_json_object_exact(root, keys, 2U) ||
      !diagnostics_state_valid(json_object_get(root, "state"), 0)) return 0;
  checks = json_object_get(root, "checks");
  return gateway_json_object_exact(checks, check_keys, 5U) &&
      diagnostics_check_valid(json_object_get(checks, "configuration"), 1) &&
      diagnostics_check_valid(json_object_get(checks, "sip_control"), 1) &&
      diagnostics_check_valid(json_object_get(checks, "media_readiness"), 0) &&
      diagnostics_check_valid(json_object_get(checks, "tls_profile_policy"), 1) &&
      diagnostics_check_valid(json_object_get(checks, "renderer_truth"), 0);
}

static int operation_backend_valid(json_t *root) {
  static const char *const keys[] = {"ok"};
  return gateway_json_object_exact(root, keys, 1U) && json_is_boolean(json_object_get(root, "ok")) &&
      json_is_true(json_object_get(root, "ok"));
}

int gateway_serialize_json(json_t *value, char *output, size_t capacity) {
  char *serialized = json_dumps(value, JSON_COMPACT | JSON_SORT_KEYS);
  if (serialized == NULL || strlen(serialized) + 1U > capacity) {
    gateway_secure_json_free(serialized);
    return 0;
  }
  (void)snprintf(output, capacity, "%s", serialized);
  gateway_secure_json_free(serialized);
  return 1;
}

int ls200_gateway_backend_response_normalize(uint8_t opcode, const char *input, char *output, size_t capacity) {
  json_error_t error;
  json_t *root;
  int valid;
  if (input == NULL || output == NULL || capacity == 0U || opcode < 1U ||
      opcode > LS200_CONTROL_OPCODE_METRICS || strlen(input) > GATEWAY_LSZ1_MAX_PAYLOAD) return 0;
  root = json_loads(input, JSON_REJECT_DUPLICATES, &error);
  if (root == NULL) return 0;
  valid = opcode == LS200_CONTROL_OPCODE_STATUS ? gateway_status_backend_valid(root) :
      opcode == LS200_CONTROL_OPCODE_DIAGNOSTICS ? diagnostics_backend_valid(root) :
      opcode == LS200_CONTROL_OPCODE_SUBSCRIBE ? gateway_event_snapshot_backend_valid(root) :
      opcode == LS200_CONTROL_OPCODE_SETTINGS ? gateway_settings_backend_valid(root) :
      opcode == LS200_CONTROL_OPCODE_METRICS ? gateway_metrics_backend_valid(root) :
      operation_backend_valid(root);
  if (valid) valid = gateway_serialize_json(root, output, capacity);
  json_decref(root);
  return valid;
}

int gateway_opaque_export_id(const char *path) {
  const char *prefix = "/zoom/api/v1/diagnostics/export/";
  size_t index, length = strlen(prefix);
  if (strncmp(path, prefix, length) != 0 || strlen(path + length) != 32U) return 0;
  for (index = length; path[index] != '\0'; ++index)
    if (!isxdigit((unsigned char)path[index])) return 0;
  return 1;
}

static int passthrough_status_path(const char *path) {
  return strcmp(path, "/zoom/api/v1/status") == 0 || strcmp(path, "/zoom/api/v1/calls/active") == 0;
}

static json_t *collection_status_data(const char *path) {
  static const struct { const char *path, *name; } collection[] = {
      {"/zoom/api/v1/calls", "recents"}, {"/zoom/api/v1/directory", "entries"}};
  size_t index;
  for (index = 0U; index < sizeof(collection) / sizeof(collection[0]); ++index)
    if (strcmp(path, collection[index].path) == 0) return json_pack("{s:[]}", collection[index].name);
  return NULL;
}

static json_t *route_status_data(const ls200_gateway *gateway, const char *path, json_t *status) {
  json_t *data = collection_status_data(path);
  if (data != NULL) return data;
  if (strcmp(path, "/zoom/api/v1/media/preview") == 0) return gateway_preview_status_data(gateway);
  if (strcmp(path, "/zoom/api/v1/media") == 0) return gateway_media_status_data(status);
  if (strcmp(path, "/zoom/api/v1/diagnostics") == 0) {
    json_t *last = json_object_get(status, "last_error");
    return json_pack("{s:s,s:s,s:s,s:s,s:s,s:O}", "last_test", "not run",
                     "result", "live status available", "console_version", "1",
                     "sip_version", "1", "sbom", "installed manifest",
                     "last_error", last != NULL ? last : json_null());
  }
  return NULL;
}

int gateway_shape_status_route(const ls200_gateway *gateway, const char *path,
                               const char *input, char *output, size_t capacity) {
  json_error_t error;
  json_t *status, *data;
  int valid;
  if (path == NULL || input == NULL || output == NULL || capacity == 0U) return 0;
  if (passthrough_status_path(path)) {
    if (output == input) return 1;
    if (strlen(input) + 1U > capacity) return 0;
    (void)snprintf(output, capacity, "%s", input);
    return 1;
  }
  status = json_loads(input, JSON_REJECT_DUPLICATES, &error);
  if (!json_is_object(status)) {
    if (status != NULL) json_decref(status);
    return 0;
  }
  data = route_status_data(gateway, path, status);
  json_decref(status);
  if (data == NULL) return 0;
  valid = gateway_serialize_json(data, output, capacity);
  json_decref(data);
  return valid;
}
