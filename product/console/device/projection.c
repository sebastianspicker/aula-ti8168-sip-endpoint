#include "projection.h"
#include <string.h>

static int activity_state(json_t *value) {
  const char *text = json_string_value(value);
  return text != NULL && (strcmp(text, "unknown") == 0 ||
      strcmp(text, "inactive") == 0 || strcmp(text, "active") == 0 ||
      strcmp(text, "paused") == 0);
}

int aula_device_status_valid(json_t *value) {
  return json_is_object(value) && json_object_size(value) == 3 &&
      json_is_integer(json_object_get(value, "revision")) &&
      json_integer_value(json_object_get(value, "revision")) == 1 &&
      activity_state(json_object_get(value, "recording")) &&
      activity_state(json_object_get(value, "streaming"));
}
