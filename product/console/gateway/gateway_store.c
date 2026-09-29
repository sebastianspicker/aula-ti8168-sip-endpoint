#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int load_account_revision(aula_gateway *gateway, json_t *root) {
  switch (json_integer_value(json_object_get(root, "revision"))) {
    case 1: return gateway_apply_loaded_account_v1(gateway, root);
    case 2: return gateway_apply_loaded_accounts_v2(gateway, root);
    case 3: return gateway_apply_loaded_store_v3(gateway, root);
    default: return 0;
  }
}

int aula_gateway_load_account(aula_gateway *gateway) {
  json_error_t error;
  json_t *root;
  int descriptor;
  int opened;
  int result = 0;
  if (gateway == NULL || gateway->config.account_store_path == NULL) return 1;
  opened = gateway_open_verified_regular(gateway->config.account_store_path, 1, &descriptor);
  if (opened == 2) return 1;
  if (opened != 1) return 0;
  root = json_loadfd(descriptor, JSON_REJECT_DUPLICATES, &error);
  (void)close(descriptor);
  if (root != NULL && json_is_object(root) && json_is_integer(json_object_get(root, "revision")))
    result = load_account_revision(gateway, root);
  if (root != NULL) json_decref(root);
  if (result != 0 && aula_gateway_account_count(gateway) != 0U)
    gateway_disable_bootstrap_code(gateway);
  return result;
}

static int reference_is_storable(const aula_gateway_safe_reference *reference) {
  return gateway_safe_reference_id(reference->id) && gateway_safe_reference_name(reference->name) &&
      gateway_safe_reference_meeting_id(reference->meeting_id) &&
      gateway_safe_reference_profile(reference->profile) &&
      gateway_safe_reference_layout(reference->default_layout);
}

static int reference_id_is_unique(const aula_gateway_safe_reference *references, size_t index) {
  size_t other;
  for (other = 0U; other < index; ++other)
    if (references[other].used && strcmp(references[other].id, references[index].id) == 0) return 0;
  return 1;
}

static json_t *safe_reference_json(const aula_gateway_safe_reference *reference) {
  return json_pack("{s:s,s:s,s:s,s:s,s:s}", "id", reference->id,
                   "name", reference->name, "meeting_id", reference->meeting_id,
                   "profile", reference->profile, "default_layout", reference->default_layout);
}

static int append_safe_references(json_t *values, const aula_gateway_safe_reference *references,
                                  size_t capacity) {
  size_t index;
  if (values == NULL || references == NULL) return 0;
  for (index = 0U; index < capacity; ++index) {
    json_t *entry;
    if (!references[index].used) continue;
    if (!reference_is_storable(&references[index]) || !reference_id_is_unique(references, index)) return 0;
    entry = safe_reference_json(&references[index]);
    if (entry == NULL) return 0;
    if (json_array_append(values, entry) != 0) {
      json_decref(entry);
      return 0;
    }
    json_decref(entry);
  }
  return 1;
}

static json_t *account_store_entry(const aula_gateway_account *account) {
  char salt[AULA_GATEWAY_SALT_BYTES * 2U + 1U] = {0};
  char password_hash_hex[AULA_GATEWAY_HASH_BYTES * 2U + 1U] = {0};
  json_t *entry = NULL;
  if (gateway_hex_encode(account->salt, sizeof(account->salt), salt, sizeof(salt)) &&
      gateway_hex_encode(account->password_hash, sizeof(account->password_hash),
                         password_hash_hex, sizeof(password_hash_hex)))
    entry = json_pack("{s:s,s:i,s:s,s:s}", "username", account->username,
                      "role", (int)account->role, "salt", salt, "password_hash", password_hash_hex);
  OPENSSL_cleanse(salt, sizeof(salt));
  OPENSSL_cleanse(password_hash_hex, sizeof(password_hash_hex));
  return entry;
}

static int append_account_entries(json_t *accounts, const aula_gateway *gateway) {
  size_t index;
  for (index = 0U; index < AULA_GATEWAY_MAX_ACCOUNTS; ++index) {
    json_t *entry;
    if (!gateway->accounts[index].configured) continue;
    entry = account_store_entry(&gateway->accounts[index]);
    if (entry == NULL) return 0;
    if (json_array_append(accounts, entry) != 0) {
      json_decref(entry);
      return 0;
    }
    json_decref(entry);
  }
  return 1;
}

static void discard_store_arrays(json_t *accounts, json_t *directory, json_t *recents) {
  if (accounts != NULL) json_decref(accounts);
  if (directory != NULL) json_decref(directory);
  if (recents != NULL) json_decref(recents);
}

static int account_store_is_valid(const aula_gateway *gateway) {
  return gateway != NULL && gateway->config.account_store_path != NULL &&
      gateway_safe_absolute_path(gateway->config.account_store_path) &&
      aula_gateway_account_count(gateway) > 0U &&
      aula_gateway_admin_count(gateway) > 0U && gateway->account_revision > 0U &&
      gateway->directory_revision > 0U;
}

static char *serialize_account_store(const aula_gateway *gateway) {
  json_t *root;
  json_t *accounts;
  json_t *directory;
  json_t *recents;
  char *serialized;
  if (!account_store_is_valid(gateway)) return NULL;
  accounts = json_array();
  directory = json_array();
  recents = json_array();
  if (accounts == NULL || directory == NULL || recents == NULL) {
    discard_store_arrays(accounts, directory, recents);
    return NULL;
  }
  if (!append_account_entries(accounts, gateway) ||
      !append_safe_references(directory, gateway->directory, AULA_GATEWAY_MAX_DIRECTORY_ENTRIES) ||
      !append_safe_references(recents, gateway->recents, AULA_GATEWAY_MAX_RECENTS)) {
    discard_store_arrays(accounts, directory, recents);
    return NULL;
  }
  root = json_pack("{s:i,s:I,s:b,s:O,s:I,s:O,s:O}", "revision", 3,
                   "account_revision", (json_int_t)gateway->account_revision,
                   "bootstrap_disabled", gateway->bootstrap_disabled, "accounts", accounts,
                   "directory_revision", (json_int_t)gateway->directory_revision,
                   "directory", directory, "recents", recents);
  discard_store_arrays(accounts, directory, recents);
  if (root == NULL) return NULL;
  serialized = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS);
  json_decref(root);
  return serialized;
}

static int create_account_temporary(int parent, const char *name, char temporary[NAME_MAX + 1U]) {
  unsigned int attempt;
  temporary[0] = '\0';
  for (attempt = 0U; attempt < 16U; ++attempt) {
    uint8_t random[8];
    char suffix[17];
    int descriptor;
    if (RAND_bytes(random, sizeof(random)) != 1 ||
        !gateway_hex_encode(random, sizeof(random), suffix, sizeof(suffix))) break;
    if (snprintf(temporary, NAME_MAX + 1U, ".%s.tmp.%s", name, suffix) < 0) break;
    descriptor = openat(parent, temporary, O_WRONLY | O_CREAT | O_EXCL | AULA_O_NOFOLLOW, 0600);
    if (descriptor >= 0 || errno != EEXIST) return descriptor;
  }
  return -1;
}

static void discard_account_temporary(int parent, int descriptor, const char *temporary) {
  if (descriptor >= 0) (void)close(descriptor);
  if (temporary[0] != '\0') (void)unlinkat(parent, temporary, 0);
}

static int write_account_temporary(int descriptor, char *serialized) {
  size_t length = strlen(serialized);
  size_t offset = 0U;
  int complete = 1;
  while (offset < length) {
    ssize_t written = write(descriptor, serialized + offset, length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      complete = 0;
      break;
    }
  }
  gateway_secure_json_free(serialized);
  return complete && gateway_sync_account_file(descriptor);
}

aula_gateway_store_result aula_gateway_store_account(const aula_gateway *gateway) {
  char name[NAME_MAX + 1U];
  char temporary[NAME_MAX + 1U];
  char *serialized;
  int descriptor = -1;
  int parent;
  serialized = serialize_account_store(gateway);
  if (serialized == NULL) return AULA_GATEWAY_STORE_NOT_COMMITTED;
  if (!gateway_open_parent_directory(gateway->config.account_store_path, &parent, name)) {
    gateway_secure_json_free(serialized);
    return AULA_GATEWAY_STORE_NOT_COMMITTED;
  }
  descriptor = create_account_temporary(parent, name, temporary);
  if (descriptor < 0 || fchmod(descriptor, 0600) != 0) {
    discard_account_temporary(parent, descriptor, temporary);
    (void)close(parent);
    gateway_secure_json_free(serialized);
    return AULA_GATEWAY_STORE_NOT_COMMITTED;
  }
  if (!write_account_temporary(descriptor, serialized)) {
    int failure = errno;
    discard_account_temporary(parent, descriptor, temporary);
    (void)close(parent);
    errno = failure;
    return AULA_GATEWAY_STORE_NOT_COMMITTED;
  }
  if (close(descriptor) != 0) {
    int failure = errno;
    descriptor = -1;
    discard_account_temporary(parent, descriptor, temporary);
    (void)close(parent);
    errno = failure;
    return AULA_GATEWAY_STORE_NOT_COMMITTED;
  }
  descriptor = -1;
  if (renameat(parent, temporary, parent, name) != 0) {
    int failure = errno;
    discard_account_temporary(parent, descriptor, temporary);
    (void)close(parent);
    errno = failure;
    return AULA_GATEWAY_STORE_NOT_COMMITTED;
  }
  if (!gateway_sync_parent_directory(parent)) {
    (void)close(parent);
    return AULA_GATEWAY_STORE_DURABILITY_UNCERTAIN;
  }
  (void)close(parent);
  return AULA_GATEWAY_STORE_DURABLE;
}

int aula_gateway_verify_password(const aula_gateway_account *account, const char *password) {
  uint8_t result[AULA_GATEWAY_HASH_BYTES] = {0};
  int valid = 0;
  if (account != NULL && account->configured &&
      gateway_password_hash_candidate(password, account->salt, result))
    valid = gateway_secure_equal(result, account->password_hash, sizeof(result));
  OPENSSL_cleanse(result, sizeof(result));
  return valid;
}
