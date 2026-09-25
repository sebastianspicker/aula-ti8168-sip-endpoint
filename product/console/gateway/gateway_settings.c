static int settings_enum_valid(const char *value, const char *const *allowed,
                               size_t allowed_count) {
  size_t index;
  if (value == NULL) return 0;
  for (index = 0U; index < allowed_count; ++index)
    if (strcmp(value, allowed[index]) == 0) return 1;
  return 0;
}

static int settings_backend_valid(json_t *root) {
  static const char *const keys[] = {"media", "profile", "revision", "tls"};
  static const char *const media[] = {"managed", "disabled"};
  static const char *const profiles[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  static const char *const tls[] = {"required", "not_required"};
  json_t *revision;
  const char *profile;
  const char *media_value;
  const char *tls_value;
  if (!json_object_exact(root, keys, 4U)) return 0;
  revision = json_object_get(root, "revision");
  profile = json_string_value(json_object_get(root, "profile"));
  media_value = json_string_value(json_object_get(root, "media"));
  tls_value = json_string_value(json_object_get(root, "tls"));
  return json_is_integer(revision) && json_integer_value(revision) >= 1 &&
      (uint64_t)json_integer_value(revision) <= UINT_MAX &&
      settings_enum_valid(profile, profiles, 3U) &&
      settings_enum_valid(media_value, media, 2U) &&
      settings_enum_valid(tls_value, tls, 2U);
}
