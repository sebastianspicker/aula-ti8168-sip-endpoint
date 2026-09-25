static int status_enum(json_t *object, const char *key,
                       const char *const *values, size_t count) {
  const char *value = json_string_value(json_object_get(object, key));
  size_t index;
  if (value == NULL) return 0;
  for (index = 0U; index < count; ++index) {
    if (strcmp(value, values[index]) == 0) return 1;
  }
  return 0;
}

static int status_integer_range(json_t *object, const char *key,
                                json_int_t minimum, json_int_t maximum) {
  json_t *value = json_object_get(object, key);
  return json_is_integer(value) && json_integer_value(value) >= minimum &&
      json_integer_value(value) <= maximum;
}

static int status_counter(json_t *object, const char *key) {
  return status_integer_range(object, key, 0, INT64_MAX);
}

static int status_profile_level_valid(json_t *video) {
  const char *value = json_string_value(json_object_get(video, "h264_profile_level_id"));
  size_t index;
  if (value == NULL) return 0;
  if (strcmp(value, "none") == 0) return 1;
  if (strlen(value) != 6U) return 0;
  for (index = 0U; index < 6U; ++index) {
    if (!isxdigit((unsigned char)value[index])) return 0;
  }
  return 1;
}

static int status_sip_valid(json_t *sip) {
  static const char *const keys[] = {
      "profile", "transport", "registration", "status_code", "expires_seconds"};
  static const char *const profiles[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  static const char *const transports[] = {"udp", "tcp", "tls"};
  static const char *const registrations[] = {"disabled", "registering", "registered", "failed"};
  return json_object_exact(sip, keys, 5U) &&
      status_enum(sip, "profile", profiles, 3U) &&
      status_enum(sip, "transport", transports, 3U) &&
      status_enum(sip, "registration", registrations, 4U) &&
      status_integer_range(sip, "status_code", 0, 699) &&
      status_integer_range(sip, "expires_seconds", 0, UINT32_MAX);
}

static int status_media_valid(json_t *media, int video) {
  static const char *const video_keys[] = {
      "payload_type", "direction", "security", "h264_profile_level_id",
      "frames", "packets", "lost", "jitter_ns"};
  static const char *const audio_keys[] = {
      "payload_type", "direction", "security", "frames", "packets", "lost", "jitter_ns"};
  static const char *const directions[] = {"sendrecv", "sendonly", "recvonly", "inactive"};
  static const char *const security[] = {"rtp", "srtp"};
  return json_object_exact(media, video != 0 ? video_keys : audio_keys,
                           video != 0 ? 8U : 7U) &&
      status_integer_range(media, "payload_type", 0, 127) &&
      status_enum(media, "direction", directions, 4U) &&
      status_enum(media, "security", security, 2U) &&
      (video == 0 || status_profile_level_valid(media)) &&
      status_counter(media, "frames") && status_counter(media, "packets") &&
      status_counter(media, "lost") && status_counter(media, "jitter_ns");
}

static int status_render_stream_valid(json_t *stream, int renderer_healthy) {
  static const char *const keys[] = {"rendering", "fresh", "last_success_ns"};
  int rendering;
  int fresh;
  if (!json_object_exact(stream, keys, 3U) ||
      !json_is_boolean(json_object_get(stream, "rendering")) ||
      !json_is_boolean(json_object_get(stream, "fresh")) ||
      !status_counter(stream, "last_success_ns")) return 0;
  rendering = json_is_true(json_object_get(stream, "rendering"));
  fresh = json_is_true(json_object_get(stream, "fresh"));
  return rendering == fresh && (rendering == 0 || renderer_healthy != 0) &&
      (fresh == 0 || json_integer_value(json_object_get(stream, "last_success_ns")) > 0);
}

static int status_renderer_valid(json_t *renderer, int *out_audio, int *out_video) {
  static const char *const keys[] = {"healthy", "audio", "video"};
  int healthy;
  json_t *audio;
  json_t *video;
  if (!json_object_exact(renderer, keys, 3U) ||
      !json_is_boolean(json_object_get(renderer, "healthy"))) return 0;
  healthy = json_is_true(json_object_get(renderer, "healthy"));
  audio = json_object_get(renderer, "audio");
  video = json_object_get(renderer, "video");
  if (!status_render_stream_valid(audio, healthy) ||
      !status_render_stream_valid(video, healthy)) return 0;
  *out_audio = json_is_true(json_object_get(audio, "rendering"));
  *out_video = json_is_true(json_object_get(video, "rendering"));
  return 1;
}

static int status_core_fields_valid(json_t *root, int audio_rendering,
                                    int video_rendering) {
  int aggregate = json_is_true(json_object_get(root, "rx_rendering"));
  return json_is_boolean(json_object_get(root, "rx_rendering")) &&
      aggregate == (audio_rendering != 0 || video_rendering != 0) &&
      status_integer_range(root, "revision", 1, 1) &&
      status_integer_range(root, "reconnect_attempts", 0, UINT32_MAX) &&
      json_is_boolean(json_object_get(root, "video_transmit_enabled")) &&
      json_is_boolean(json_object_get(root, "audio_muted")) &&
      status_integer_range(root, "last_error", -10, 4);
}

static int status_backend_valid(json_t *root) {
  static const char *const keys[] = {
      "call_state", "media_state", "revision", "rx_rendering",
      "reconnect_attempts", "video_transmit_enabled", "audio_muted", "renderer", "aec",
      "sip", "video", "audio", "last_error"};
  static const char *const call_states[] = {
      "idle", "resolving", "inviting", "early", "establishing_media",
      "established", "terminating", "backing_off", "failed", "terminated",
      "stopped", "terminal_failure"};
  static const char *const media_states[] = {
      "none", "new", "reserved", "preparing", "prepared", "committed", "stopped"};
  int audio_rendering = 0;
  int video_rendering = 0;
  if (!json_object_exact(root, keys, 13U) ||
      !status_renderer_valid(json_object_get(root, "renderer"),
                             &audio_rendering, &video_rendering)) return 0;
  return
      status_enum(root, "call_state", call_states, 12U) &&
      status_enum(root, "media_state", media_states, 7U) &&
      status_core_fields_valid(root, audio_rendering, video_rendering) &&
      aec_status_valid(json_object_get(root, "aec")) &&
      status_sip_valid(json_object_get(root, "sip")) &&
      status_media_valid(json_object_get(root, "video"), 1) &&
      status_media_valid(json_object_get(root, "audio"), 0);
}

static const char *media_security_status(json_t *status) {
  const char *video = json_string_value(
      json_object_get(json_object_get(status, "video"), "security"));
  const char *audio = json_string_value(
      json_object_get(json_object_get(status, "audio"), "security"));
  if (strcmp(video, "srtp") == 0 && strcmp(audio, "srtp") == 0) return "srtp";
  if (strcmp(video, "srtp") == 0 || strcmp(audio, "srtp") == 0) return "mixed srtp/rtp";
  return "rtp";
}

static json_t *media_status_data(json_t *status) {
  const char *aec = json_string_value(
      json_object_get(json_object_get(status, "aec"), "delay_state"));
  json_t *renderer = json_object_get(status, "renderer");
  int healthy = json_is_true(json_object_get(renderer, "healthy"));
  int audio = json_is_true(json_object_get(json_object_get(renderer, "audio"), "rendering")) &&
      json_is_true(json_object_get(json_object_get(renderer, "audio"), "fresh"));
  int video = json_is_true(json_object_get(json_object_get(renderer, "video"), "rendering")) &&
      json_is_true(json_object_get(json_object_get(renderer, "video"), "fresh"));
  return json_pack(
      "{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
      "video_codec", "H.264", "audio_codec", "G.711",
      "rtp", media_security_status(status), "rtcp", "reported by sipd",
      "renderer", healthy != 0 ? "healthy" : "unavailable",
      "audio_renderer", audio != 0 ? "healthy" : "not rendering",
      "video_renderer", video != 0 ? "healthy" : "not rendering",
      "hdmi", "not reported", "aec", aec);
}
