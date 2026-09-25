#include "projection.h"
#include <string.h>

static int activity_state(json_t *value) {
  const char *text = json_string_value(value);
  return text != NULL && (strcmp(text, "unknown") == 0 ||
      strcmp(text, "inactive") == 0 || strcmp(text, "active") == 0 ||
      strcmp(text, "paused") == 0);
}

int ls200_device_status_valid(json_t *value) {
  return json_is_object(value) && json_object_size(value) == 3 &&
      json_is_integer(json_object_get(value, "revision")) &&
      json_integer_value(json_object_get(value, "revision")) == 1 &&
      activity_state(json_object_get(value, "recording")) &&
      activity_state(json_object_get(value, "streaming"));
}

static int recorder_stream(json_t *stream, int *file, int *network) {
  const char *destination = json_string_value(json_object_get(stream, "destination"));
  json_t *error = json_object_get(stream, "error");
  if (!json_is_object(stream) || destination == NULL || strlen(destination) > 2048 ||
      !json_is_integer(error) || json_integer_value(error) != 0) return 0;
  if (strncmp(destination, "file://", 7) == 0) *file = 1;
  else if (strncmp(destination, "rtmp://", 7) == 0 ||
           strncmp(destination, "rtmps://", 8) == 0 ||
           strncmp(destination, "srt://", 6) == 0) *network = 1;
  /* RTSP may be the local preview. Other destinations are ambiguous. */
  else if (strncmp(destination, "rtsp://", 7) != 0) return 0;
  return 1;
}

static const char *recorder_state(json_t *raw) {
  const char *state = json_string_value(json_object_get(raw, "state"));
  if (state == NULL) return NULL;
  /* The recovered OEM state map wraps its values in literal quotes. Keep
   * accepting the unquoted fixture/other firmware representation as well. */
  if (strcmp(state, "\"Stopped\"") == 0) return "Stopped";
  if (strcmp(state, "\"Recording\"") == 0) return "Recording";
  if (strcmp(state, "\"Paused\"") == 0) return "Paused";
  return state;
}

json_t *ls200_device_project_recorder(json_t *raw) {
  const char *state = recorder_state(raw);
  json_t *streams = json_object_get(raw, "streams");
  const char *recording = "unknown", *streaming = "unknown";
  int file = 0, network = 0, valid = 1;
  size_t index;
  if (state == NULL || !json_is_array(streams) || json_array_size(streams) > 32)
    goto finish;
  for (index = 0; index < json_array_size(streams); ++index)
    if (!recorder_stream(json_array_get(streams, index), &file, &network)) valid = 0;
  if (!valid) goto finish;
  if (strcmp(state, "Stopped") == 0 && json_array_size(streams) == 0) {
    recording = "inactive"; streaming = "inactive";
  } else if (strcmp(state, "Recording") == 0 || strcmp(state, "Paused") == 0) {
    recording = file ? (strcmp(state, "Paused") == 0 ? "paused" : "active") : "inactive";
    streaming = network ? "active" : "inactive";
  }
finish:
  return json_pack("{s:i,s:s,s:s}", "revision", 1,
                   "recording", recording, "streaming", streaming);
}
