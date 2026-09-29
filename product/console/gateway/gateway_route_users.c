#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

static const char *role_name(aula_gateway_role role) {
  return role == AULA_GATEWAY_ROLE_ADMIN ? "admin" :
      role == AULA_GATEWAY_ROLE_OPERATOR ? "operator" :
      role == AULA_GATEWAY_ROLE_VIEWER ? "viewer" : NULL;
}

static int parse_role(const char *value, aula_gateway_role *role) {
  if (value == NULL || role == NULL) return 0;
  if (strcmp(value, "admin") == 0) *role = AULA_GATEWAY_ROLE_ADMIN;
  else if (strcmp(value, "operator") == 0) *role = AULA_GATEWAY_ROLE_OPERATOR;
  else if (strcmp(value, "viewer") == 0) *role = AULA_GATEWAY_ROLE_VIEWER;
  else return 0;
  return 1;
}

static int account_revision_matches(const aula_gateway *gateway, json_t *root) {
  json_t *revision = json_object_get(root, "revision");
  return json_is_integer(revision) && json_integer_value(revision) > 0 &&
      (uint64_t)json_integer_value(revision) <= INT_MAX &&
      (unsigned int)json_integer_value(revision) == gateway->account_revision;
}

int gateway_write_users(aula_gateway *gateway, aula_gateway_response *response) {
  json_t *users = json_array();
  json_t *data;
  size_t index;
  char serialized[1024];
  if (users == NULL) return 0;
  for (index = 0U; index < AULA_GATEWAY_MAX_ACCOUNTS; ++index) {
    const aula_gateway_account *account = &gateway->accounts[index];
    json_t *entry;
    if (!account->configured) continue;
    entry = json_pack("{s:s,s:s}", "username", account->username, "role", role_name(account->role));
    if (entry == NULL) {
      json_decref(users);
      return 0;
    }
    {
      int appended = json_array_append(users, entry);
      json_decref(entry);
      if (appended != 0) {
        json_decref(users);
        return 0;
      }
    }
  }
  data = json_pack("{s:i,s:O}", "revision", (int)gateway->account_revision, "users", users);
  json_decref(users);
  if (data == NULL || !gateway_serialize_json(data, serialized, sizeof(serialized))) {
    if (data != NULL) json_decref(data);
    return 0;
  }
  json_decref(data);
  gateway_write_success(response, 200U, serialized);
  return 1;
}

static int user_mutation_schema(json_t *root, int deleting, const char **username,
                                const char **password, aula_gateway_role *role) {
  *username = json_string_value(json_object_get(root, "username"));
  if (!json_is_object(root) || json_object_size(root) != (deleting ? 2U : 4U) ||
      json_object_get(root, "username") == NULL || json_object_get(root, "revision") == NULL ||
      (!deleting && (json_object_get(root, "password") == NULL || json_object_get(root, "role") == NULL)) ||
      !gateway_is_safe_username(*username)) return 0;
  if (deleting) return 1;
  *password = json_string_value(json_object_get(root, "password"));
  return *password != NULL && strlen(*password) <= 256U &&
      parse_role(json_string_value(json_object_get(root, "role")), role);
}

static int user_mutation_precondition(const aula_gateway *gateway, const aula_gateway_account *existing,
                                      int deleting, aula_gateway_role role,
                                      aula_gateway_response *response) {
  if (deleting && existing == NULL) {
    gateway_write_error(response, 404U, "USER_NOT_FOUND", "account is not configured");
    return 0;
  }
  if (!deleting && existing == NULL && aula_gateway_account_count(gateway) >= AULA_GATEWAY_MAX_ACCOUNTS) {
    gateway_write_error(response, 409U, "ACCOUNT_CAPACITY", "account capacity is reached");
    return 0;
  }
  if (existing != NULL && existing->role == AULA_GATEWAY_ROLE_ADMIN &&
      (deleting || role != AULA_GATEWAY_ROLE_ADMIN) && aula_gateway_admin_count(gateway) == 1U) {
    gateway_write_error(response, 409U, "LAST_ADMIN", "at least one administrator is required");
    return 0;
  }
  return 1;
}

static int apply_user_mutation(aula_gateway *gateway, int deleting, const char *username,
                               const char *password, aula_gateway_role role) {
  if (deleting) return aula_gateway_delete_account(gateway, username);
  return aula_gateway_upsert_account(gateway, username, password, role);
}

int gateway_handle_user_mutation(aula_gateway *gateway, aula_gateway_session *session,
                                 const aula_gateway_request *request,
                                 aula_gateway_response *response, char revoked_username[33]) {
  json_error_t error;
  json_t *root = gateway_parse_json(request->body, &error);
  const char *username = NULL, *password = NULL;
  const aula_gateway_account *existing;
  aula_gateway_role role = AULA_GATEWAY_ROLE_VIEWER;
  int deleting = strcmp(request->method, "DELETE") == 0;
  int had_existing;
  aula_gateway_store_result store_result;
  aula_gateway_account backup_accounts[AULA_GATEWAY_MAX_ACCOUNTS];
  aula_gateway_account backup_legacy;
  unsigned int backup_revision = 0U;
  char data[128], target_username[33];
  revoked_username[0] = '\0';
  if (root == NULL || !user_mutation_schema(root, deleting, &username, &password, &role)) {
    if (root != NULL) json_decref(root);
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return 1;
  }
  (void)snprintf(target_username, sizeof(target_username), "%s", username);
  if (!account_revision_matches(gateway, root)) {
    json_decref(root);
    gateway_write_error(response, 409U, "REVISION_CONFLICT", "account revision is stale");
    return 1;
  }
  existing = aula_gateway_find_account(gateway, target_username);
  had_existing = existing != NULL;
  if (!user_mutation_precondition(gateway, existing, deleting, role, response)) {
    json_decref(root);
    return 1;
  }
  if (gateway->account_revision == (unsigned int)INT_MAX) {
    json_decref(root);
    gateway_write_error(response, 409U, "REVISION_CONFLICT", "account revision cannot advance");
    return 1;
  }
  (void)memcpy(backup_accounts, gateway->accounts, sizeof(backup_accounts));
  backup_legacy = gateway->account;
  backup_revision = gateway->account_revision;
  if (!apply_user_mutation(gateway, deleting, target_username, password, role)) {
    (void)memcpy(gateway->accounts, backup_accounts, sizeof(backup_accounts));
    gateway->account = backup_legacy;
    gateway->account_revision = backup_revision;
    json_decref(root);
    OPENSSL_cleanse(backup_accounts, sizeof(backup_accounts));
    OPENSSL_cleanse(&backup_legacy, sizeof(backup_legacy));
    gateway_write_error(response, 400U, "SCHEMA_INVALID", "body schema is invalid");
    return 1;
  }
  ++gateway->account_revision;
  store_result = aula_gateway_store_account(gateway);
  if (store_result == AULA_GATEWAY_STORE_NOT_COMMITTED) {
    (void)memcpy(gateway->accounts, backup_accounts, sizeof(backup_accounts));
    gateway->account = backup_legacy;
    gateway->account_revision = backup_revision;
    json_decref(root);
    OPENSSL_cleanse(backup_accounts, sizeof(backup_accounts));
    OPENSSL_cleanse(&backup_legacy, sizeof(backup_legacy));
    gateway_write_error(response, 500U, "PERSISTENCE_FAILED", "account state was not saved");
    return 1;
  }
  json_decref(root);
  OPENSSL_cleanse(backup_accounts, sizeof(backup_accounts));
  OPENSSL_cleanse(&backup_legacy, sizeof(backup_legacy));
  if (had_existing) (void)snprintf(revoked_username, 33U, "%s", target_username);
  if (store_result == AULA_GATEWAY_STORE_DURABILITY_UNCERTAIN) {
    gateway_write_error(response, 503U, "PERSISTENCE_UNCERTAIN",
                        "account was committed but restart durability was not confirmed");
    return 2;
  }
  (void)snprintf(data, sizeof(data), "{\"username\":\"%s\",\"role\":\"%s\",\"revision\":%u}",
                 target_username, deleting ? "deleted" : role_name(role), gateway->account_revision);
  gateway_write_success(response, deleting ? 200U : (existing == NULL ? 201U : 200U), data);
  (void)session;
  return 2;
}
