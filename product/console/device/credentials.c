#include "credentials.h"
#include <limits.h>
#include <string.h>

/* Shared with credentials_store.c's on-disk document validation; declared in
 * credentials.h since both translation units need it. */
int ls200_device_credential_text(json_t *value, size_t maximum) {
  const char *text = json_string_value(value);
  size_t length = json_string_length(value);
  if (text == NULL || length == 0 || length > maximum || strlen(text) != length) return 0;
  /* OEM splits authorization at every colon and accepts only the first pair. */
  for (size_t i = 0; i < length; ++i)
    if ((unsigned char)text[i] < 32 || text[i] == 127 || text[i] == ':') return 0;
  return 1;
}

int ls200_device_credentials_schema(json_t *arguments) {
  json_t *revision = json_object_get(arguments, "revision");
  return json_is_object(arguments) && json_object_size(arguments) == 3 &&
      json_is_integer(revision) && json_integer_value(revision) >= 1 &&
      json_integer_value(revision) < INT_MAX &&
      ls200_device_credential_text(json_object_get(arguments, "username"), 64) &&
      ls200_device_credential_text(json_object_get(arguments, "password"), 128);
}
