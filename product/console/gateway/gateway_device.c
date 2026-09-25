static int device_correlation(const gateway_transaction *transaction, char value[65]) {
  char identity[400];
  uint8_t hash[32];
  const char *key = transaction->request->idempotency_key;
  int length = snprintf(identity, sizeof(identity), "%s\n%s\n%s",
      transaction->principal.username, transaction->request->path, key == NULL ? "read" : key);
  if (length < 0 || (size_t)length >= sizeof(identity) ||
      !sha256((const uint8_t *)identity, (size_t)length, hash)) return 0;
  return hex_encode(hash, sizeof(hash), value, 65);
}

static int oem_status_projection(json_t *data) {
  static const char *const keys[] = {"revision", "credentials_present", "connection"};
  static const char *const states[] = {"unknown", "connected", "authentication_failed", "backoff", "unconfigured"};
  const char *connection;
  json_t *revision = json_object_get(data, "revision");
  if (!json_object_exact(data, keys, 3) || !json_is_integer(revision) ||
      json_integer_value(revision) < 1 || json_integer_value(revision) > 33 ||
      !json_is_boolean(json_object_get(data, "credentials_present"))) return 0;
  connection = json_string_value(json_object_get(data, "connection"));
  if (connection == NULL) return 0;
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
    if (strcmp(connection, states[i]) == 0) return 1;
  return 0;
}

static int device_reply_code(json_t *reply, const char *operation, const char *correlation) {
  static const char *const keys[] = {"revision", "operation", "correlation", "outcome", "data"};
  static const char *const outcomes[] = {"succeeded", "revision_conflict", "idempotency_conflict", "capacity", "persistence_uncertain", "unavailable", "invalid"};
  const char *op = json_string_value(json_object_get(reply, "operation"));
  const char *id = json_string_value(json_object_get(reply, "correlation"));
  const char *outcome = json_string_value(json_object_get(reply, "outcome"));
  json_t *revision = json_object_get(reply, "revision");
  if (!json_object_exact(reply, keys, 5) || !json_is_integer(revision) ||
      json_integer_value(revision) != 2 || op == NULL || id == NULL || outcome == NULL) return 0;
  if (strcmp(op, operation) || strcmp(id, correlation)) return 0;
  for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i)
    if (strcmp(outcome, outcomes[i]) == 0) return (int)i + 1;
  return 0;
}

static int device_backend_reply(const gateway_transaction *transaction,
    char *backend, size_t capacity) {
  json_error_t error;
  json_t *arguments, *request, *reply;
  char correlation[65], *encoded = NULL;
  int code;
  int mutation = (transaction->plan.flags & R_MUTATION) != 0;
  const char *operation = mutation ? "credentials.replace" : "credentials.status";
  if (strcmp(transaction->request->path, "/zoom/api/v1/device/status") == 0)
    return ls200_device_read_status(backend, capacity);
  if (!device_correlation(transaction, correlation)) return 0;
  arguments = mutation ? json_loads(transaction->request->body, JSON_REJECT_DUPLICATES, &error) : json_object();
  if (arguments == NULL) return 0;
  request = json_pack("{s:i,s:s,s:s,s:O}", "revision", 2, "operation", operation,
      "correlation", correlation, "arguments", arguments);
  reply = request == NULL ? NULL : ls200_device_exchange(request);
  json_decref(arguments); json_decref(request);
  code = device_reply_code(reply, operation, correlation);
  if (code == 1) {
    json_t *data = json_object_get(reply, "data");
    if (oem_status_projection(data)) encoded = json_dumps(data, JSON_COMPACT);
    if (encoded == NULL || strlen(encoded) >= capacity) code = 0;
    else snprintf(backend, capacity, "%s", encoded);
  } else if (!json_is_null(json_object_get(reply, "data"))) code = 0;
  free(encoded); json_decref(reply);
  return code;
}

static void finish_device_reply(int code, const char *backend, ls200_gateway_response *response) {
  switch (code) {
    case 1: write_success(response, 200U, backend); break;
    case 2: write_error(response, 409U, "REVISION_CONFLICT", "device settings changed; refresh before saving"); break;
    case 3: write_error(response, 409U, "IDEMPOTENCY_CONFLICT", "operation identifier was already used"); break;
    case 4: write_error(response, 409U, "STATE_CAPACITY", "persistent operation capacity is reached"); break;
    case 5: write_error(response, 503U, "PERSISTENCE_UNCERTAIN", "restart durability was not confirmed"); break;
    case 7: write_error(response, 400U, "SCHEMA_INVALID", "device request is invalid"); break;
    default: write_error(response, 503U, "DEVICE_UNAVAILABLE", "device control is unavailable"); break;
  }
}
