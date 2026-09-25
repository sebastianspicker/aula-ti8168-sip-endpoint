#include "gateway_internal.h"

#include <ctype.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

static int load_account_value(ls200_gateway_account *account, json_t *value);

static unsigned int account_values_admin_count(
    const ls200_gateway_account accounts[LS200_GATEWAY_MAX_ACCOUNTS]) {
  unsigned int count = 0U;
  size_t index;
  for (index = 0U; index < LS200_GATEWAY_MAX_ACCOUNTS; ++index)
    if (accounts[index].configured && accounts[index].role == LS200_GATEWAY_ROLE_ADMIN)
      ++count;
  return count;
}

static int load_account_values(ls200_gateway_account accounts[LS200_GATEWAY_MAX_ACCOUNTS], json_t *values) {
  size_t index;
  if (!json_is_array(values) || json_array_size(values) == 0U ||
      json_array_size(values) > LS200_GATEWAY_MAX_ACCOUNTS) return 0;
  (void)memset(accounts, 0, sizeof(ls200_gateway_account) * LS200_GATEWAY_MAX_ACCOUNTS);
  for (index = 0U; index < json_array_size(values); ++index) {
    size_t other;
    if (!load_account_value(&accounts[index], json_array_get(values, index))) return 0;
    for (other = 0U; other < index; ++other)
      if (strcmp(accounts[other].username, accounts[index].username) == 0) return 0;
  }
  return account_values_admin_count(accounts) > 0U;
}

static int load_safe_reference(ls200_gateway_safe_reference *reference, json_t *value) {
  static const char *const keys[] = {"default_layout", "id", "meeting_id", "name", "profile"};
  const char *id;
  const char *name;
  const char *meeting_id;
  const char *profile;
  const char *default_layout;
  if (reference == NULL || !gateway_json_object_exact(value, keys, 5U)) return 0;
  id = json_string_value(json_object_get(value, "id"));
  name = json_string_value(json_object_get(value, "name"));
  meeting_id = json_string_value(json_object_get(value, "meeting_id"));
  profile = json_string_value(json_object_get(value, "profile"));
  default_layout = json_string_value(json_object_get(value, "default_layout"));
  if (!gateway_safe_reference_id(id) || !gateway_safe_reference_name(name) ||
      !gateway_safe_reference_meeting_id(meeting_id) || !gateway_safe_reference_profile(profile) ||
      !gateway_safe_reference_layout(default_layout)) return 0;
  (void)memset(reference, 0, sizeof(*reference));
  reference->used = 1;
  (void)snprintf(reference->id, sizeof(reference->id), "%s", id);
  (void)snprintf(reference->name, sizeof(reference->name), "%s", name);
  (void)snprintf(reference->meeting_id, sizeof(reference->meeting_id), "%s", meeting_id);
  (void)snprintf(reference->profile, sizeof(reference->profile), "%s", profile);
  (void)snprintf(reference->default_layout, sizeof(reference->default_layout), "%s", default_layout);
  return 1;
}

static int load_safe_references(ls200_gateway_safe_reference *references, size_t capacity, json_t *values) {
  size_t index;
  if (references == NULL || !json_is_array(values) || json_array_size(values) > capacity)
    return 0;
  (void)memset(references, 0, sizeof(*references) * capacity);
  for (index = 0U; index < json_array_size(values); ++index) {
    size_t other;
    if (!load_safe_reference(&references[index], json_array_get(values, index))) return 0;
    for (other = 0U; other < index; ++other)
      if (strcmp(references[other].id, references[index].id) == 0) return 0;
  }
  return 1;
}

int gateway_apply_loaded_account_v1(ls200_gateway *gateway, json_t *root) {
  static const char *const keys[] = {"bootstrap_disabled", "password_hash", "revision", "role", "salt", "username"};
  ls200_gateway_account account = {0};
  json_t *role;
  const char *username;
  const char *salt;
  const char *stored_hash;
  int result = 0;
  if (gateway == NULL || root == NULL || !gateway_json_object_exact(root, keys, 6U) ||
      !json_is_integer(json_object_get(root, "revision")) ||
      json_integer_value(json_object_get(root, "revision")) != 1 ||
      !json_is_boolean(json_object_get(root, "bootstrap_disabled"))) {
    goto cleanup;
  }
  username = json_string_value(json_object_get(root, "username"));
  salt = json_string_value(json_object_get(root, "salt"));
  stored_hash = json_string_value(json_object_get(root, "password_hash"));
  role = json_object_get(root, "role");
  if (!gateway_is_safe_username(username) || !json_is_integer(role) ||
      json_integer_value(role) < LS200_GATEWAY_ROLE_VIEWER ||
      json_integer_value(role) > LS200_GATEWAY_ROLE_ADMIN ||
      !gateway_hex_decode(salt, account.salt, LS200_GATEWAY_SALT_BYTES) ||
      !gateway_hex_decode_32(stored_hash, account.password_hash)) goto cleanup;
  account.configured = 1;
  account.role = (ls200_gateway_role)json_integer_value(role);
  (void)snprintf(account.username, sizeof(account.username), "%s", username);
  (void)memset(gateway->accounts, 0, sizeof(gateway->accounts));
  gateway->accounts[0] = account;
  gateway->account = account;
  gateway->bootstrap_disabled = json_is_true(json_object_get(root, "bootstrap_disabled"));
  gateway->account_revision = 1U;
  gateway->directory_revision = 1U;
  (void)memset(gateway->directory, 0, sizeof(gateway->directory));
  (void)memset(gateway->recents, 0, sizeof(gateway->recents));
  result = 1;
cleanup:
  OPENSSL_cleanse(&account, sizeof(account));
  return result;
}

static int load_account_value(ls200_gateway_account *account, json_t *value) {
  static const char *const keys[] = {"password_hash", "role", "salt", "username"};
  const char *username;
  const char *salt;
  const char *stored_hash;
  json_t *role;
  if (account == NULL || !gateway_json_object_exact(value, keys, 4U)) return 0;
  username = json_string_value(json_object_get(value, "username"));
  salt = json_string_value(json_object_get(value, "salt"));
  stored_hash = json_string_value(json_object_get(value, "password_hash"));
  role = json_object_get(value, "role");
  if (!gateway_is_safe_username(username) || !json_is_integer(role) ||
      json_integer_value(role) < LS200_GATEWAY_ROLE_VIEWER ||
      json_integer_value(role) > LS200_GATEWAY_ROLE_ADMIN ||
      !gateway_hex_decode(salt, account->salt, LS200_GATEWAY_SALT_BYTES) ||
      !gateway_hex_decode_32(stored_hash, account->password_hash)) return 0;
  account->configured = 1;
  account->role = (ls200_gateway_role)json_integer_value(role);
  (void)snprintf(account->username, sizeof(account->username), "%s", username);
  return 1;
}

int gateway_apply_loaded_accounts_v2(ls200_gateway *gateway, json_t *root) {
  static const char *const keys[] = {"account_revision", "accounts", "bootstrap_disabled", "revision"};
  ls200_gateway_account accounts[LS200_GATEWAY_MAX_ACCOUNTS] = {{0}};
  int result = 0;
  if (gateway == NULL || !gateway_json_object_exact(root, keys, 4U) ||
      !json_is_integer(json_object_get(root, "revision")) ||
      json_integer_value(json_object_get(root, "revision")) != 2 ||
      !json_is_integer(json_object_get(root, "account_revision")) ||
      json_integer_value(json_object_get(root, "account_revision")) < 1 ||
      (uint64_t)json_integer_value(json_object_get(root, "account_revision")) > UINT_MAX ||
      !json_is_boolean(json_object_get(root, "bootstrap_disabled"))) goto cleanup;
  if (!load_account_values(accounts, json_object_get(root, "accounts"))) goto cleanup;
  (void)memcpy(gateway->accounts, accounts, sizeof(accounts));
  gateway->bootstrap_disabled = json_is_true(json_object_get(root, "bootstrap_disabled"));
  gateway->account_revision = (unsigned int)json_integer_value(json_object_get(root, "account_revision"));
  gateway->directory_revision = 1U;
  (void)memset(gateway->directory, 0, sizeof(gateway->directory));
  (void)memset(gateway->recents, 0, sizeof(gateway->recents));
  gateway_sync_legacy_account(gateway);
  result = 1;
cleanup:
  OPENSSL_cleanse(accounts, sizeof(accounts));
  return result;
}

int gateway_apply_loaded_store_v3(ls200_gateway *gateway, json_t *root) {
  static const char *const keys[] = {"account_revision", "accounts", "bootstrap_disabled", "directory",
      "directory_revision", "recents", "revision"};
  ls200_gateway_account accounts[LS200_GATEWAY_MAX_ACCOUNTS] = {{0}};
  ls200_gateway_safe_reference directory[LS200_GATEWAY_MAX_DIRECTORY_ENTRIES] = {{0}};
  ls200_gateway_safe_reference recents[LS200_GATEWAY_MAX_RECENTS] = {{0}};
  json_t *account_revision;
  json_t *directory_revision;
  int result = 0;
  if (gateway == NULL || !gateway_json_object_exact(root, keys, 7U) ||
      !json_is_integer(json_object_get(root, "revision")) ||
      json_integer_value(json_object_get(root, "revision")) != 3 ||
      !json_is_boolean(json_object_get(root, "bootstrap_disabled"))) goto cleanup;
  account_revision = json_object_get(root, "account_revision");
  directory_revision = json_object_get(root, "directory_revision");
  if (!json_is_integer(account_revision) || json_integer_value(account_revision) < 1 ||
      (uint64_t)json_integer_value(account_revision) > UINT_MAX ||
      !json_is_integer(directory_revision) || json_integer_value(directory_revision) < 1 ||
      (uint64_t)json_integer_value(directory_revision) > UINT_MAX ||
      !load_account_values(accounts, json_object_get(root, "accounts")) ||
      !load_safe_references(directory, LS200_GATEWAY_MAX_DIRECTORY_ENTRIES,
                            json_object_get(root, "directory")) ||
      !load_safe_references(recents, LS200_GATEWAY_MAX_RECENTS,
                            json_object_get(root, "recents"))) goto cleanup;
  (void)memcpy(gateway->accounts, accounts, sizeof(accounts));
  (void)memcpy(gateway->directory, directory, sizeof(directory));
  (void)memcpy(gateway->recents, recents, sizeof(recents));
  gateway->bootstrap_disabled = json_is_true(json_object_get(root, "bootstrap_disabled"));
  gateway->account_revision = (unsigned int)json_integer_value(account_revision);
  gateway->directory_revision = (unsigned int)json_integer_value(directory_revision);
  gateway_sync_legacy_account(gateway);
  result = 1;
cleanup:
  OPENSSL_cleanse(accounts, sizeof(accounts));
  OPENSSL_cleanse(directory, sizeof(directory));
  OPENSSL_cleanse(recents, sizeof(recents));
  return result;
}
