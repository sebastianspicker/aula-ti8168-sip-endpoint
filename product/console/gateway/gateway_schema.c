#include "gateway_internal.h"

#include <string.h>

int gateway_no_body_schema(const char *body) {
  json_error_t error;
  if (body == NULL || body[0] == '\0') return 1;
  json_t *root = gateway_parse_json(body, &error);
  int valid = root != NULL && json_is_object(root) && json_object_size(root) == 0U;
  if (root != NULL) json_decref(root);
  return valid;
}

int gateway_schema_keys(const char *body, const char *const *keys, size_t key_count) {
  json_error_t error;
  json_t *root = gateway_parse_json(body, &error);
  int valid = root != NULL && gateway_json_object_exact(root, keys, key_count);
  if (root != NULL) json_decref(root);
  return valid;
}

int gateway_schema_enum(const char *body, const char *key, const char *const *values, size_t value_count) {
  json_error_t error;
  json_t *root = gateway_parse_json(body, &error);
  const char *value;
  size_t index;
  if (root == NULL) return 0;
  value = json_string_value(json_object_get(root, key));
  for (index = 0U; value != NULL && index < value_count; ++index)
    if (strcmp(value, values[index]) == 0) { json_decref(root); return 1; }
  json_decref(root);
  return 0;
}
