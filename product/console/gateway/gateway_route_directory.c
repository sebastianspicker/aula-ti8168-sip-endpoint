#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <ctype.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

static int directory_id(const char *value) {
  size_t index;
  if (value == NULL || value[0] == '\0' || strlen(value) > 32U) return 0;
  for (index = 0U; value[index] != '\0'; ++index)
    if ((unsigned char)value[index] < 0x21U || (unsigned char)value[index] > 0x7eU ||
        (!isalnum((unsigned char)value[index]) && value[index] != '_' && value[index] != '-')) return 0;
  return 1;
}

static int directory_name_character_is_safe(unsigned char character) {
  return character >= 0x20U && character <= 0x7eU && character != '/' &&
      character != '@' && character != '%' && character != '\\';
}

static int directory_name_has_sip_scheme(const char *value, size_t index, size_t length) {
  return index + 3U < length && tolower((unsigned char)value[index]) == 's' &&
      tolower((unsigned char)value[index + 1U]) == 'i' &&
      tolower((unsigned char)value[index + 2U]) == 'p' && value[index + 3U] == ':';
}

static int directory_name(const char *value) {
  size_t index, length;
  if (value == NULL || value[0] == '\0' || strlen(value) > 64U) return 0;
  length = strlen(value);
  for (index = 0U; value[index] != '\0'; ++index) {
    if (!directory_name_character_is_safe((unsigned char)value[index]) ||
        directory_name_has_sip_scheme(value, index, length)) return 0;
  }
  return 1;
}

static int directory_profile(const char *value) {
  return value != NULL && (strcmp(value, "zoom_direct") == 0 ||
      strcmp(value, "zoom_proxy") == 0 || strcmp(value, "private_lab") == 0);
}

static int directory_layout(const char *value) {
  return value != NULL && (strcmp(value, "gallery") == 0 ||
      strcmp(value, "full_screen") == 0 || strcmp(value, "dual_video") == 0);
}

static int directory_meeting(const char *value) {
  return value != NULL && strlen(value) >= 9U && strlen(value) <= 11U && gateway_digits_only(value, 0);
}

static aula_gateway_safe_reference *find_reference(aula_gateway_safe_reference *entries, const char *id) {
  size_t i;
  for (i = 0U; i < AULA_GATEWAY_MAX_DIRECTORY_ENTRIES; ++i)
    if (entries[i].used && strcmp(entries[i].id, id) == 0) return &entries[i];
  return NULL;
}

static int serialize_references(const aula_gateway_safe_reference *entries, size_t count,
                                unsigned int revision, const char *name,
                                char output[AULA_GATEWAY_COLLECTION_RESPONSE_BYTES]) {
  json_t *items = json_array();
  json_t *data;
  size_t i;
  int valid;
  if (items == NULL) return 0;
  for (i = 0U; i < count; ++i) {
    json_t *item;
    int appended;
    if (!entries[i].used) continue;
    item = json_pack("{s:s,s:s,s:s,s:s,s:s}", "id", entries[i].id, "name", entries[i].name,
                     "meeting_id", entries[i].meeting_id, "profile", entries[i].profile,
                     "default_layout", entries[i].default_layout);
    if (item == NULL) {
      json_decref(items);
      return 0;
    }
    appended = json_array_append(items, item);
    json_decref(item);
    if (appended != 0) {
      json_decref(items);
      return 0;
    }
  }
  data = json_pack("{s:i,s:O}", "revision", (int)revision, name, items);
  json_decref(items);
  if (data == NULL) return 0;
  valid = gateway_serialize_json(data, output, AULA_GATEWAY_COLLECTION_RESPONSE_BYTES);
  json_decref(data);
  return valid;
}

int gateway_write_directory(const aula_gateway *gateway, aula_gateway_response *response) {
  char data[AULA_GATEWAY_COLLECTION_RESPONSE_BYTES];
  if (!serialize_references(gateway->directory, AULA_GATEWAY_MAX_DIRECTORY_ENTRIES,
                            gateway->directory_revision, "entries", data)) return 0;
  gateway_write_success(response, 200U, data);
  return 1;
}

int gateway_write_recents(const aula_gateway *gateway, aula_gateway_response *response) {
  char data[AULA_GATEWAY_COLLECTION_RESPONSE_BYTES];
  if (!serialize_references(gateway->recents, AULA_GATEWAY_MAX_RECENTS, 1U, "recents", data)) return 0;
  gateway_write_success(response, 200U, data);
  return 1;
}

static int directory_mutation_schema(json_t *root, int deleting, aula_gateway_safe_reference *value) {
  const char *id, *name, *meeting, *profile, *layout;
  if (!json_is_object(root) || json_object_size(root) != (deleting ? 2U : 6U)) return 0;
  id = json_string_value(json_object_get(root, "id"));
  if (!directory_id(id) || json_object_get(root, "revision") == NULL) return 0;
  if (deleting) {
    (void)snprintf(value->id, sizeof(value->id), "%s", id);
    return 1;
  }
  name = json_string_value(json_object_get(root, "name"));
  meeting = json_string_value(json_object_get(root, "meeting_id"));
  profile = json_string_value(json_object_get(root, "profile"));
  layout = json_string_value(json_object_get(root, "default_layout"));
  if (!directory_name(name) || !directory_meeting(meeting) || !directory_profile(profile) ||
      !directory_layout(layout)) return 0;
  value->used = 1;
  (void)snprintf(value->id, sizeof(value->id), "%s", id);
  (void)snprintf(value->name, sizeof(value->name), "%s", name);
  (void)snprintf(value->meeting_id, sizeof(value->meeting_id), "%s", meeting);
  (void)snprintf(value->profile, sizeof(value->profile), "%s", profile);
  (void)snprintf(value->default_layout, sizeof(value->default_layout), "%s", layout);
  return 1;
}

static int directory_revision_matches(const aula_gateway *gateway, json_t *root) {
  json_t *revision = json_object_get(root, "revision");
  return json_is_integer(revision) && json_integer_value(revision) == (json_int_t)gateway->directory_revision;
}

static aula_gateway_safe_reference *first_free_reference(aula_gateway *gateway) {
  size_t index;
  for (index = 0U; index < AULA_GATEWAY_MAX_DIRECTORY_ENTRIES; ++index)
    if (!gateway->directory[index].used) return &gateway->directory[index];
  return NULL;
}

static int directory_mutation_target(aula_gateway *gateway, int deleting,
                                     aula_gateway_safe_reference *existing,
                                     aula_gateway_safe_reference **target,
                                     aula_gateway_response *response) {
  if (deleting && existing == NULL) {
    gateway_write_error(response, 404U, "DIRECTORY_NOT_FOUND", "entry is not configured");
    return 0;
  }
  *target = existing;
  if (!deleting && *target == NULL) *target = first_free_reference(gateway);
  if (!deleting && *target == NULL) {
    gateway_write_error(response, 409U, "DIRECTORY_CAPACITY", "directory capacity is reached");
    return 0;
  }
  return 1;
}

int gateway_handle_directory_mutation(aula_gateway *gateway, const aula_gateway_request *request,
                                      aula_gateway_response *response) {
  json_error_t error;
  json_t *root = gateway_parse_json(request->body, &error);
  aula_gateway_safe_reference value, *existing, *target;
  aula_gateway_safe_reference backup[AULA_GATEWAY_MAX_DIRECTORY_ENTRIES];
  unsigned int backup_revision = 0U;
  aula_gateway_store_result store_result = AULA_GATEWAY_STORE_DURABLE;
  int deleting = strcmp(request->method, "DELETE") == 0;
  char data[160];
  (void)memset(&value, 0, sizeof(value));
  if (root == NULL || !directory_mutation_schema(root, deleting, &value)) {
    if (root != NULL) json_decref(root);
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return 1;
  }
  if (!directory_revision_matches(gateway, root)) {
    json_decref(root);
    gateway_write_error(response, 409U, "REVISION_CONFLICT", "directory revision is stale");
    return 1;
  }
  json_decref(root);
  existing = find_reference(gateway->directory, value.id);
  if (!directory_mutation_target(gateway, deleting, existing, &target, response)) return 1;
  if (gateway->directory_revision == (unsigned int)INT_MAX) {
    gateway_write_error(response, 409U, "REVISION_CONFLICT", "directory revision cannot advance");
    return 1;
  }
  (void)memcpy(backup, gateway->directory, sizeof(backup));
  backup_revision = gateway->directory_revision;
  if (deleting) OPENSSL_cleanse(existing, sizeof(*existing));
  else *target = value;
  ++gateway->directory_revision;
  if (gateway->config.account_store_path != NULL) store_result = aula_gateway_store_account(gateway);
  if (store_result == AULA_GATEWAY_STORE_NOT_COMMITTED) {
    (void)memcpy(gateway->directory, backup, sizeof(backup));
    gateway->directory_revision = backup_revision;
    OPENSSL_cleanse(backup, sizeof(backup));
    gateway_write_error(response, 500U, "PERSISTENCE_FAILED", "directory state was not saved");
    return 1;
  }
  OPENSSL_cleanse(backup, sizeof(backup));
  if (store_result == AULA_GATEWAY_STORE_DURABILITY_UNCERTAIN) {
    gateway_write_error(response, 503U, "PERSISTENCE_UNCERTAIN",
                        "directory was committed but restart durability was not confirmed");
    return 2;
  }
  (void)snprintf(data, sizeof(data), "{\"id\":\"%s\",\"revision\":%u}", value.id, gateway->directory_revision);
  gateway_write_success(response, deleting ? 200U : (existing == NULL ? 201U : 200U), data);
  return 2;
}

int gateway_record_recent(aula_gateway *gateway, const char *body) {
  json_error_t error;
  json_t *root = json_loads(body, JSON_REJECT_DUPLICATES, &error);
  aula_gateway_safe_reference value;
  uint8_t random[16];
  aula_gateway_safe_reference backup[AULA_GATEWAY_MAX_RECENTS];
  aula_gateway_store_result store_result = AULA_GATEWAY_STORE_DURABLE;
  if (root == NULL) return 0;
  (void)memset(&value, 0, sizeof(value));
  if (!directory_meeting(json_string_value(json_object_get(root, "meeting_id"))) ||
      !directory_profile(json_string_value(json_object_get(root, "profile"))) ||
      !directory_layout(json_string_value(json_object_get(root, "layout"))) ||
      RAND_bytes(random, sizeof(random)) != 1 ||
      !gateway_hex_encode(random, sizeof(random), value.id, sizeof(value.id))) {
    json_decref(root);
    OPENSSL_cleanse(random, sizeof(random));
    return 0;
  }
  (void)snprintf(value.name, sizeof(value.name), "Meeting %s",
                 json_string_value(json_object_get(root, "meeting_id")));
  (void)snprintf(value.meeting_id, sizeof(value.meeting_id), "%s",
                 json_string_value(json_object_get(root, "meeting_id")));
  (void)snprintf(value.profile, sizeof(value.profile), "%s",
                 json_string_value(json_object_get(root, "profile")));
  (void)snprintf(value.default_layout, sizeof(value.default_layout), "%s",
                 json_string_value(json_object_get(root, "layout")));
  value.used = 1;
  json_decref(root);
  OPENSSL_cleanse(random, sizeof(random));
  (void)memcpy(backup, gateway->recents, sizeof(backup));
  (void)memmove(&gateway->recents[1], &gateway->recents[0],
               sizeof(gateway->recents) - sizeof(gateway->recents[0]));
  gateway->recents[0] = value;
  if (gateway->config.account_store_path != NULL) store_result = aula_gateway_store_account(gateway);
  if (store_result == AULA_GATEWAY_STORE_NOT_COMMITTED) {
    (void)memcpy(gateway->recents, backup, sizeof(backup));
    OPENSSL_cleanse(backup, sizeof(backup));
    return 0;
  }
  OPENSSL_cleanse(backup, sizeof(backup));
  return store_result == AULA_GATEWAY_STORE_DURABILITY_UNCERTAIN ? 2 : 1;
}
