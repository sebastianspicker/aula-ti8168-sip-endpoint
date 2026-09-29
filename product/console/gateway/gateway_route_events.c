#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int event_message_valid(const char *message) {
  static const char *const states[] = {
      "idle", "resolving", "inviting", "early", "establishing_media", "established",
      "terminating", "backing_off", "failed", "terminated", "stopped", "terminal_failure"};
  size_t index;
  if (message == NULL) return 0;
  for (index = 0U; index < sizeof(states) / sizeof(states[0]); ++index)
    if (strcmp(message, states[index]) == 0) return 1;
  return 0;
}

static int event_id_valid(const char *id) {
  size_t length, index;
  if (id == NULL || strncmp(id, "call-state-", 11U) != 0) return 0;
  length = strlen(id);
  if (length < 12U || length > 32U) return 0;
  for (index = 11U; index < length; ++index)
    if (!isdigit((unsigned char)id[index])) return 0;
  return 1;
}

static int event_object_valid(json_t *event) {
  static const char *const keys[] = {"id", "type", "message"};
  const char *type, *message;
  if (!gateway_json_object_exact(event, keys, 3U)) return 0;
  type = json_string_value(json_object_get(event, "type"));
  message = json_string_value(json_object_get(event, "message"));
  return event_id_valid(json_string_value(json_object_get(event, "id"))) &&
      type != NULL && strcmp(type, "call_state") == 0 && event_message_valid(message);
}

int gateway_event_snapshot_backend_valid(json_t *root) {
  static const char *const keys[] = {"revision", "events"};
  json_t *revision, *events;
  size_t index;
  if (!gateway_json_object_exact(root, keys, 2U) || !gateway_json_value_is_safe(root)) return 0;
  revision = json_object_get(root, "revision");
  events = json_object_get(root, "events");
  if (!json_is_integer(revision) || json_integer_value(revision) < 1 || !json_is_array(events) ||
      json_array_size(events) > AULA_GATEWAY_MAX_EVENTS) return 0;
  for (index = 0U; index < json_array_size(events); ++index)
    if (!event_object_valid(json_array_get(events, index))) return 0;
  return 1;
}

static int event_already_recorded(const aula_gateway *gateway, const char *id,
                                  const char *type, const char *message) {
  size_t slot;
  for (slot = 0U; slot < AULA_GATEWAY_MAX_EVENTS; ++slot) {
    if (!gateway->events[slot].used || strcmp(gateway->events[slot].id, id) != 0) continue;
    return strcmp(gateway->events[slot].type, type) == 0 &&
        strcmp(gateway->events[slot].message, message) == 0 ? 1 : -1;
  }
  return 0;
}

int gateway_record_events(aula_gateway *gateway, const char *body) {
  json_error_t error;
  json_t *root = json_loads(body, JSON_REJECT_DUPLICATES, &error);
  json_t *events;
  size_t i;
  if (root == NULL) return 0;
  events = json_object_get(root, "events");
  for (i = 0U; i < json_array_size(events); ++i) {
    json_t *item = json_array_get(events, i);
    const char *id = json_string_value(json_object_get(item, "id"));
    const char *type = json_string_value(json_object_get(item, "type"));
    const char *message = json_string_value(json_object_get(item, "message"));
    int recorded = event_already_recorded(gateway, id, type, message);
    if (recorded < 0) {
      json_decref(root);
      return 0;
    }
    if (recorded > 0) continue;
    (void)memmove(&gateway->events[1], &gateway->events[0],
                  sizeof(gateway->events) - sizeof(gateway->events[0]));
    (void)memset(&gateway->events[0], 0, sizeof(gateway->events[0]));
    gateway->events[0].used = 1;
    (void)snprintf(gateway->events[0].id, sizeof(gateway->events[0].id), "%s", id);
    (void)snprintf(gateway->events[0].type, sizeof(gateway->events[0].type), "%s", type);
    (void)snprintf(gateway->events[0].message, sizeof(gateway->events[0].message), "%s", message);
  }
  json_decref(root);
  return 1;
}

int gateway_write_events(const aula_gateway *gateway, aula_gateway_response *response) {
  json_t *events = json_array();
  json_t *data;
  char serialized[AULA_GATEWAY_COLLECTION_RESPONSE_BYTES];
  size_t i;
  if (events == NULL) return 0;
  for (i = 0U; i < AULA_GATEWAY_MAX_EVENTS; ++i) {
    json_t *item;
    int appended;
    if (!gateway->events[i].used) continue;
    item = json_pack("{s:s,s:s,s:s}", "id", gateway->events[i].id,
                     "type", gateway->events[i].type, "message", gateway->events[i].message);
    if (item == NULL) {
      json_decref(events);
      return 0;
    }
    appended = json_array_append(events, item);
    json_decref(item);
    if (appended != 0) {
      json_decref(events);
      return 0;
    }
  }
  data = json_pack("{s:O}", "events", events);
  json_decref(events);
  if (data == NULL || !gateway_serialize_json(data, serialized, sizeof(serialized))) {
    if (data != NULL) json_decref(data);
    return 0;
  }
  json_decref(data);
  gateway_write_success(response, 200U, serialized);
  return 1;
}
