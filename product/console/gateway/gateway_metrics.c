#include "gateway_internal.h"

#include <string.h>

/* Separate strict schema: never extend the revision-1 legacy status object. */
static int metrics_stream_valid(json_t *stream, const char *kind) {
  static const char *const keys[] = {"kind", "queue_depth", "packet_drops",
      "access_unit_drops", "backend_drops", "backend_restarts"};
  const char *actual;
  size_t index;
  if (!gateway_json_object_exact(stream, keys, 6U)) return 0;
  actual = json_string_value(json_object_get(stream, "kind"));
  if (actual == NULL || strcmp(actual, kind) != 0) return 0;
  for (index = 1U; index < 6U; ++index)
    if (!gateway_status_integer_range(stream, keys[index], 0, INT32_MAX)) return 0;
  return 1;
}

int gateway_metrics_backend_valid(json_t *root) {
  static const char *const keys[] = {"revision", "reset", "streams", "poll"};
  static const char *const reset_keys[] = {"streams", "poll"};
  static const char *const poll_keys[] = {"late_count", "max_lateness_ms"};
  json_t *reset = json_object_get(root, "reset");
  json_t *streams = json_object_get(root, "streams");
  json_t *poll = json_object_get(root, "poll");
  const char *stream_reset = json_string_value(json_object_get(reset, "streams"));
  const char *poll_reset = json_string_value(json_object_get(reset, "poll"));
  return gateway_json_object_exact(root, keys, 4U) &&
      gateway_status_integer_range(root, "revision", 1, 1) &&
      gateway_json_object_exact(reset, reset_keys, 2U) && stream_reset != NULL &&
      strcmp(stream_reset, "session") == 0 && poll_reset != NULL &&
      strcmp(poll_reset, "process") == 0 && json_is_array(streams) &&
      json_array_size(streams) == 2U &&
      metrics_stream_valid(json_array_get(streams, 0U), "audio") &&
      metrics_stream_valid(json_array_get(streams, 1U), "video") &&
      gateway_json_object_exact(poll, poll_keys, 2U) &&
      gateway_status_integer_range(poll, "late_count", 0, INT32_MAX) &&
      gateway_status_integer_range(poll, "max_lateness_ms", 0, INT32_MAX);
}
