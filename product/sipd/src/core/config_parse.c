#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "config_private.h"
#include "ls200_sipd/media_aec.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int ls200_config_has_control_or_parent_segment(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  const char *segment = value;
  while (*cursor != '\0') {
    if (*cursor < 0x20U || *cursor == 0x7fU) {
      return 1;
    }
    if (*cursor == (unsigned char)'/') {
      if ((size_t)(cursor - (const unsigned char *)segment) == 2U &&
          segment[0] == '.' && segment[1] == '.') {
        return 1;
      }
      segment = (const char *)cursor + 1;
    }
    ++cursor;
  }
  return (strcmp(segment, "..") == 0) ? 1 : 0;
}

static ls200_status copy_value(char *destination, size_t capacity, const char *value,
                               int allow_empty) {
  size_t length;
  if (destination == NULL || value == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  length = strlen(value);
  if ((!allow_empty && length == 0U) || length >= capacity) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  (void)memcpy(destination, value, length + 1U);
  return LS200_STATUS_OK;
}

static ls200_status parse_u32(const char *value, uint32_t *out_value) {
  unsigned long parsed;
  char *end = NULL;
  if (value == NULL || out_value == NULL || value[0] == '\0') {
    return LS200_STATUS_INVALID_DATA;
  }
  for (end = (char *)value; *end != '\0'; ++end) {
    if (!isdigit((unsigned char)*end)) {
      return LS200_STATUS_INVALID_DATA;
    }
  }
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  *out_value = (uint32_t)parsed;
  return LS200_STATUS_OK;
}

static ls200_status parse_u16(const char *value, uint16_t *out_value) {
  uint32_t parsed;
  ls200_status status = parse_u32(value, &parsed);
  if (status != LS200_STATUS_OK || parsed > UINT16_MAX) {
    return (status == LS200_STATUS_OK) ? LS200_STATUS_LIMIT_EXCEEDED : status;
  }
  *out_value = (uint16_t)parsed;
  return LS200_STATUS_OK;
}

static ls200_status parse_octal_mode(const char *value, uint16_t *out_value) {
  unsigned long parsed;
  char *end = NULL;
  size_t index;
  if (value == NULL || out_value == NULL || strlen(value) != 4U || value[0] != '0') {
    return LS200_STATUS_INVALID_DATA;
  }
  for (index = 0U; index < 4U; ++index) {
    if (value[index] < '0' || value[index] > '7') {
      return LS200_STATUS_INVALID_DATA;
    }
  }
  errno = 0;
  parsed = strtoul(value, &end, 8);
  if (errno != 0 || end == value || *end != '\0' || parsed > 0777UL) {
    return LS200_STATUS_INVALID_DATA;
  }
  *out_value = (uint16_t)parsed;
  return LS200_STATUS_OK;
}

static ls200_status parse_boolean(const char *value, int *out_value) {
  if (strcmp(value, "true") == 0) {
    *out_value = 1;
    return LS200_STATUS_OK;
  }
  if (strcmp(value, "false") == 0) {
    *out_value = 0;
    return LS200_STATUS_OK;
  }
  return LS200_STATUS_INVALID_DATA;
}

static int parse_ipv4_octet(const char **cursor, uint8_t *out_octet) {
  unsigned int parsed = 0U;
  size_t digits = 0U;
  if (**cursor == '0' && (*cursor)[1] >= '0' && (*cursor)[1] <= '9') return 0;
  while (**cursor >= '0' && **cursor <= '9') {
    parsed = parsed * 10U + (unsigned int)(**cursor - '0');
    if (++digits > 3U || parsed > 255U) return 0;
    ++*cursor;
  }
  if (digits == 0U) return 0;
  *out_octet = (uint8_t)parsed;
  return 1;
}
int ls200_config_parse_ipv4_literal(const char *value, uint8_t octets[4]) {
  size_t index;
  const char *cursor = value;
  if (value == NULL || octets == NULL || value[0] == '\0') return 0;
  for (index = 0U; index < 4U; ++index) {
    if (!parse_ipv4_octet(&cursor, &octets[index])) return 0;
    if (index < 3U && *cursor++ != '.') return 0;
  }
  return *cursor == '\0';
}

int ls200_config_ipv4_is_loopback(const uint8_t octets[4]) {
  return octets[0] == 127U;
}

int ls200_config_ipv4_is_private_or_loopback(const uint8_t octets[4]) {
  return ls200_config_ipv4_is_loopback(octets) || octets[0] == 10U ||
         (octets[0] == 172U && octets[1] >= 16U && octets[1] <= 31U) ||
         (octets[0] == 192U && octets[1] == 168U);
}

int ls200_config_ipv4_is_authorized_rtsp_target(const uint8_t octets[4]) {
  if (octets == NULL || (octets[0] == 0U && octets[1] == 0U &&
      octets[2] == 0U && octets[3] == 0U) ||
      (octets[0] == 255U && octets[1] == 255U && octets[2] == 255U &&
       octets[3] == 255U)) return 0;
  return ls200_config_ipv4_is_private_or_loopback(octets) ||
      (octets[0] == 169U && octets[1] == 254U);
}

char *ls200_config_trim(char *value) {
  char *end;
  while (*value != '\0' && isspace((unsigned char)*value)) {
    ++value;
  }
  end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1])) {
    --end;
  }
  *end = '\0';
  return value;
}

ls200_status ls200_config_key_for(const char *section, const char *key, config_key *out_key) {
  static const struct {
    const char *section;
    const char *key;
    config_key id;
  } keys[] = {
    {"runtime", "foreground", CONFIG_RUNTIME_FOREGROUND},
    {"runtime", "pid_file", CONFIG_RUNTIME_PID_FILE},
    {"runtime", "settings_file", CONFIG_RUNTIME_SETTINGS_FILE},
    {"runtime", "log_sink", CONFIG_RUNTIME_LOG_SINK},
    {"runtime", "log_level", CONFIG_RUNTIME_LOG_LEVEL},
    {"runtime", "log_summary_interval_seconds", CONFIG_RUNTIME_LOG_SUMMARY_INTERVAL},
    {"sip", "uri", CONFIG_SIP_URI},
    {"sip", "profile", CONFIG_SIP_PROFILE},
    {"sip", "registrar_uri", CONFIG_SIP_REGISTRAR_URI},
    {"sip", "transport", CONFIG_SIP_TRANSPORT},
    {"sip", "auth_username", CONFIG_SIP_AUTH_USERNAME},
    {"sip", "auth_secret_file", CONFIG_SIP_AUTH_SECRET_FILE},
    {"sip", "tls_ca_file", CONFIG_SIP_TLS_CA_FILE},
    {"sip", "crc_address", CONFIG_SIP_CRC_ADDRESS},
    {"sip", "enable_tls", CONFIG_SIP_ENABLE_TLS},
    {"sip", "enable_public_network", CONFIG_SIP_ENABLE_PUBLIC_NETWORK},
    {"sip", "transaction_timeout_ms", CONFIG_SIP_TRANSACTION_TIMEOUT},
    {"sip", "max_retransmissions", CONFIG_SIP_MAX_RETRANSMISSIONS},
    {"sip", "max_reconnect_attempts", CONFIG_SIP_MAX_RECONNECT_ATTEMPTS},
    {"media", "backend", CONFIG_MEDIA_BACKEND}, {"media", "video_source", CONFIG_MEDIA_VIDEO_SOURCE},
    {"media", "audio_source", CONFIG_MEDIA_AUDIO_SOURCE},
    {"media", "authorized_rtsp_ipv4", CONFIG_MEDIA_AUTHORIZED_RTSP_IPV4},
    {"media", "bind_address", CONFIG_MEDIA_BIND_ADDRESS},
    {"media", "advertised_address", CONFIG_MEDIA_ADVERTISED_ADDRESS},
    {"media", "preferred_audio_codec", CONFIG_MEDIA_PREFERRED_AUDIO_CODEC},
    {"media", "security", CONFIG_MEDIA_SECURITY},
    {"media", "rtsp_use_udp", CONFIG_MEDIA_RTSP_USE_UDP},
    {"media", "rtsp_jitter_max_ms", CONFIG_MEDIA_RTSP_JITTER_MAX_MS},
    {"media", "rtsp_reconnect_limit", CONFIG_MEDIA_RTSP_RECONNECT_LIMIT},
    {"media", "local_rtp_port_min", CONFIG_MEDIA_LOCAL_RTP_PORT_MIN},
    {"media", "local_rtp_port_max", CONFIG_MEDIA_LOCAL_RTP_PORT_MAX},
    {"media", "rtp_mtu", CONFIG_MEDIA_RTP_MTU},
    {"media", "aec_enabled", CONFIG_MEDIA_AEC_ENABLED},
    {"media", "aec_reference_delay_frames", CONFIG_MEDIA_AEC_REFERENCE_DELAY_FRAMES},
    {"media", "aec_delay_calibrated", CONFIG_MEDIA_AEC_DELAY_CALIBRATED},
    {"media", "video_queue_frames", CONFIG_MEDIA_VIDEO_QUEUE_FRAMES},
    {"media", "audio_queue_frames", CONFIG_MEDIA_AUDIO_QUEUE_FRAMES},
    {"media", "child_uid", CONFIG_MEDIA_CHILD_UID},
    {"media", "child_gid", CONFIG_MEDIA_CHILD_GID},
    {"control", "enable_local_control", CONFIG_CONTROL_ENABLE_LOCAL_CONTROL},
    {"control", "unix_socket_path", CONFIG_CONTROL_UNIX_SOCKET_PATH},
    {"control", "unix_socket_mode", CONFIG_CONTROL_UNIX_SOCKET_MODE},
    {"control", "gateway_uid", CONFIG_CONTROL_GATEWAY_UID},
    {"limits", "sip_message_bytes", CONFIG_LIMITS_SIP_MESSAGE_BYTES},
    {"limits", "sdp_bytes", CONFIG_LIMITS_SDP_BYTES},
    {"limits", "rtp_packet_bytes", CONFIG_LIMITS_RTP_PACKET_BYTES},
    {"limits", "video_access_unit_bytes", CONFIG_LIMITS_VIDEO_ACCESS_UNIT_BYTES},
    {"limits", "audio_frame_bytes", CONFIG_LIMITS_AUDIO_FRAME_BYTES},
    {"limits", "control_message_bytes", CONFIG_LIMITS_CONTROL_MESSAGE_BYTES},
    {"limits", "log_events_per_interval", CONFIG_LIMITS_LOG_EVENTS_PER_INTERVAL}
  };
  size_t index;
  for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); ++index) {
    if (strcmp(section, keys[index].section) == 0 && strcmp(key, keys[index].key) == 0) {
      *out_key = keys[index].id;
      return LS200_STATUS_OK;
    }
  }
  return LS200_STATUS_CONFIGURATION_ERROR;
}

static ls200_status parse_nonzero_u32(const char *value, uint32_t maximum, uint32_t *out) {
  ls200_status status = parse_u32(value, out);
  return status != LS200_STATUS_OK || *out == 0U || *out > maximum ? LS200_STATUS_INVALID_DATA : LS200_STATUS_OK;
}
static ls200_status apply_runtime_key(ls200_config *config, config_key key, const char *value) {
  uint32_t parsed;
  if (key == CONFIG_RUNTIME_FOREGROUND) return parse_boolean(value, &config->view.foreground);
  if (key == CONFIG_RUNTIME_PID_FILE) return copy_value(config->pid_file, sizeof(config->pid_file), value, 0);
  if (key == CONFIG_RUNTIME_SETTINGS_FILE) return copy_value(
      config->runtime_settings_file, sizeof(config->runtime_settings_file), value, 1);
  if (key == CONFIG_RUNTIME_LOG_SUMMARY_INTERVAL) { ls200_status status = parse_nonzero_u32(value, UINT32_MAX, &parsed); if (status == LS200_STATUS_OK) config->view.log_summary_interval_seconds = parsed; return status; }
  if (key == CONFIG_RUNTIME_LOG_SINK) { if (strcmp(value, "stderr") == 0) { config->view.log_sink = LS200_LOG_SINK_STDERR; return LS200_STATUS_OK; } if (strcmp(value, "syslog") == 0) { config->view.log_sink = LS200_LOG_SINK_SYSLOG; return LS200_STATUS_OK; } }
  if (key == CONFIG_RUNTIME_LOG_LEVEL) { static const char *const names[] = {"debug", "info", "notice", "warning", "error"}; size_t index; for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) if (strcmp(value, names[index]) == 0) { config->view.log_level = (ls200_log_level)index; return LS200_STATUS_OK; } }
  return LS200_STATUS_INVALID_DATA;
}
static ls200_status apply_sip_identity_key(ls200_config *config, config_key key, const char *value) {
  if (key == CONFIG_SIP_CRC_ADDRESS) return copy_value(config->crc_address, sizeof(config->crc_address), value, 1);
  if (key == CONFIG_SIP_URI) return copy_value(config->sip_uri, sizeof(config->sip_uri), value, 1);
  if (key == CONFIG_SIP_PROFILE) {
    if (strcmp(value, "zoom_direct") != 0 && strcmp(value, "zoom_proxy") != 0 &&
        strcmp(value, "private_lab") != 0) return LS200_STATUS_INVALID_DATA;
    return copy_value(config->sip_profile, sizeof(config->sip_profile), value, 0);
  }
  if (key == CONFIG_SIP_REGISTRAR_URI) return copy_value(config->sip_registrar_uri, sizeof(config->sip_registrar_uri), value, 1);
  if (key == CONFIG_SIP_AUTH_USERNAME) return copy_value(config->auth_username, sizeof(config->auth_username), value, 1);
  if (key == CONFIG_SIP_AUTH_SECRET_FILE) return copy_value(config->auth_secret_file, sizeof(config->auth_secret_file), value, 1);
  if (key == CONFIG_SIP_TLS_CA_FILE) return copy_value(config->tls_ca_file, sizeof(config->tls_ca_file), value, 1);
  if (key == CONFIG_SIP_TRANSPORT) { static const char *const names[] = {"udp", "tcp", "tls"}; size_t index; for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) if (strcmp(value, names[index]) == 0) { config->view.transport = (ls200_transport)index; return LS200_STATUS_OK; } return LS200_STATUS_INVALID_DATA; }
  return LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_sip_flag_key(ls200_config *config, config_key key, const char *value) {
  if (key == CONFIG_SIP_ENABLE_TLS) return parse_boolean(value, &config->view.enable_tls);
  return key == CONFIG_SIP_ENABLE_PUBLIC_NETWORK ? parse_boolean(value, &config->view.enable_public_network) : LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_sip_limit_key(ls200_config *config, config_key key, const char *value) {
  uint32_t parsed;
  if (key == CONFIG_SIP_TRANSACTION_TIMEOUT) { ls200_status status = parse_nonzero_u32(value, 300000U, &parsed); if (status == LS200_STATUS_OK) config->transaction_timeout_ms = parsed; return status; }
  if (key == CONFIG_SIP_MAX_RETRANSMISSIONS || key == CONFIG_SIP_MAX_RECONNECT_ATTEMPTS) { ls200_status status = parse_u32(value, &parsed); if (status != LS200_STATUS_OK || parsed > 32U) return LS200_STATUS_INVALID_DATA; if (key == CONFIG_SIP_MAX_RETRANSMISSIONS) config->max_retransmissions = parsed; else config->view.limits.reconnect_attempts = parsed; return LS200_STATUS_OK; }
  return LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_sip_key(ls200_config *config, config_key key, const char *value) {
  if (key == CONFIG_SIP_CRC_ADDRESS || key == CONFIG_SIP_URI || key == CONFIG_SIP_PROFILE || key == CONFIG_SIP_REGISTRAR_URI || key == CONFIG_SIP_TRANSPORT || key == CONFIG_SIP_AUTH_USERNAME || key == CONFIG_SIP_AUTH_SECRET_FILE || key == CONFIG_SIP_TLS_CA_FILE) return apply_sip_identity_key(config, key, value);
  if (key == CONFIG_SIP_ENABLE_TLS || key == CONFIG_SIP_ENABLE_PUBLIC_NETWORK) return apply_sip_flag_key(config, key, value);
  return apply_sip_limit_key(config, key, value);
}

static ls200_status apply_media_backend_key(ls200_config *config, const char *value) {
  if (strcmp(value, "fixture") != 0 && strcmp(value, "rtsp_native") != 0 &&
      strcmp(value, "rtsp_gst_process") != 0)
    return LS200_STATUS_INVALID_DATA;
  return copy_value(config->media_backend, sizeof(config->media_backend), value, 0);
}

static ls200_status apply_media_source_key(ls200_config *config, config_key key,
                                           const char *value) {
  if (key == CONFIG_MEDIA_VIDEO_SOURCE)
    return copy_value(config->video_source, sizeof(config->video_source), value, 1);
  if (key == CONFIG_MEDIA_AUDIO_SOURCE)
    return copy_value(config->audio_source, sizeof(config->audio_source), value, 1);
  if (key == CONFIG_MEDIA_AUTHORIZED_RTSP_IPV4)
    return copy_value(config->authorized_rtsp_ipv4, sizeof(config->authorized_rtsp_ipv4), value, 0);
  if (key == CONFIG_MEDIA_BIND_ADDRESS)
    return copy_value(config->media_bind_address, sizeof(config->media_bind_address), value, 0);
  if (key == CONFIG_MEDIA_ADVERTISED_ADDRESS)
    return copy_value(config->media_advertised_address, sizeof(config->media_advertised_address), value, 0);
  return LS200_STATUS_CONFIGURATION_ERROR;
}

static ls200_status apply_media_audio_codec(const char *value, ls200_g711_codec *out_codec) {
  if (strcmp(value, "pcmu") == 0) {
    *out_codec = LS200_G711_PCMU;
    return LS200_STATUS_OK;
  }
  if (strcmp(value, "pcma") == 0) {
    *out_codec = LS200_G711_PCMA;
    return LS200_STATUS_OK;
  }
  return LS200_STATUS_INVALID_DATA;
}

static ls200_status apply_media_security_policy(const char *value,
                                                ls200_media_security_policy *out_policy) {
  if (strcmp(value, "prefer_srtp") == 0) {
    *out_policy = PREFER_SRTP;
    return LS200_STATUS_OK;
  }
  if (strcmp(value, "required") == 0) {
    *out_policy = REQUIRE_SRTP;
    return LS200_STATUS_OK;
  }
  if (strcmp(value, "plain_compat") == 0) {
    *out_policy = PLAIN_COMPAT;
    return LS200_STATUS_OK;
  }
  return LS200_STATUS_INVALID_DATA;
}

static ls200_status apply_media_text_key(ls200_config *config, config_key key, const char *value) {
  if (key == CONFIG_MEDIA_BACKEND) return apply_media_backend_key(config, value);
  if (key >= CONFIG_MEDIA_VIDEO_SOURCE && key <= CONFIG_MEDIA_ADVERTISED_ADDRESS)
    return apply_media_source_key(config, key, value);
  if (key == CONFIG_MEDIA_PREFERRED_AUDIO_CODEC)
    return apply_media_audio_codec(value, &config->view.preferred_audio_codec);
  if (key == CONFIG_MEDIA_SECURITY)
    return apply_media_security_policy(value, &config->view.media_security_policy);
  return LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_media_rtsp_key(ls200_config *config, config_key key,
                                         const char *value) {
  uint32_t parsed;
  ls200_status status;
  if (key == CONFIG_MEDIA_RTSP_USE_UDP) return parse_boolean(value, &config->view.rtsp_use_udp);
  status = parse_u32(value, &parsed);
  if (status != LS200_STATUS_OK) return status;
  if (key == CONFIG_MEDIA_RTSP_JITTER_MAX_MS) {
    if (parsed == 0U || parsed > 2000U) return LS200_STATUS_INVALID_DATA;
    config->view.rtsp_jitter_max_ms = parsed;
    return LS200_STATUS_OK;
  }
  if (key == CONFIG_MEDIA_RTSP_RECONNECT_LIMIT) {
    if (parsed > 32U) return LS200_STATUS_INVALID_DATA;
    config->view.rtsp_reconnect_limit = parsed;
    return LS200_STATUS_OK;
  }
  return LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_media_u16_key(ls200_config *config, config_key key, const char *value) {
  uint16_t parsed; ls200_status status = parse_u16(value, &parsed);
  if (status != LS200_STATUS_OK) return status;
  if (key == CONFIG_MEDIA_LOCAL_RTP_PORT_MIN) config->view.local_rtp_port_min = parsed;
  else if (key == CONFIG_MEDIA_LOCAL_RTP_PORT_MAX) config->view.local_rtp_port_max = parsed;
  else if (key == CONFIG_MEDIA_RTP_MTU) config->view.rtp_mtu = parsed;
  else return LS200_STATUS_CONFIGURATION_ERROR;
  return LS200_STATUS_OK;
}
static ls200_status apply_media_aec_key(ls200_config *config, config_key key,
                                        const char *value) {
  uint32_t delay;
  if (key == CONFIG_MEDIA_AEC_ENABLED) return parse_boolean(value, &config->view.aec_enabled);
  if (key == CONFIG_MEDIA_AEC_DELAY_CALIBRATED) {
    return parse_boolean(value, &config->view.aec_delay_calibrated);
  }
  if (parse_u32(value, &delay) != LS200_STATUS_OK || delay > LS200_AEC_MAX_DELAY_FRAMES) {
    return LS200_STATUS_INVALID_DATA;
  }
  config->view.aec_reference_delay_frames = delay;
  return LS200_STATUS_OK;
}
static ls200_status apply_media_u32_key(ls200_config *config, config_key key, const char *value) {
  uint32_t parsed; ls200_status status = parse_u32(value, &parsed);
  if (status != LS200_STATUS_OK) return status;
  if (key == CONFIG_MEDIA_VIDEO_QUEUE_FRAMES) config->view.limits.video_queue_frames = parsed;
  else if (key == CONFIG_MEDIA_AUDIO_QUEUE_FRAMES) config->view.limits.audio_queue_frames = parsed;
  else if (key == CONFIG_MEDIA_CHILD_UID) config->view.media_child_uid = parsed;
  else if (key == CONFIG_MEDIA_CHILD_GID) config->view.media_child_gid = parsed;
  else return LS200_STATUS_CONFIGURATION_ERROR;
  return LS200_STATUS_OK;
}
static ls200_status apply_media_key(ls200_config *config, config_key key, const char *value) {
  if (key <= CONFIG_MEDIA_SECURITY) return apply_media_text_key(config, key, value);
  if (key <= CONFIG_MEDIA_RTSP_RECONNECT_LIMIT) return apply_media_rtsp_key(config, key, value);
  if (key <= CONFIG_MEDIA_RTP_MTU) return apply_media_u16_key(config, key, value);
  if (key <= CONFIG_MEDIA_AEC_DELAY_CALIBRATED) return apply_media_aec_key(config, key, value);
  return apply_media_u32_key(config, key, value);
}
static ls200_status apply_control_key(ls200_config *config, config_key key, const char *value) {
  uint32_t parsed;
  if (key == CONFIG_CONTROL_ENABLE_LOCAL_CONTROL) return parse_boolean(value, &config->view.enable_local_control);
  if (key == CONFIG_CONTROL_UNIX_SOCKET_PATH) return copy_value(config->local_control_socket, sizeof(config->local_control_socket), value, 0);
  if (key == CONFIG_CONTROL_UNIX_SOCKET_MODE) return parse_octal_mode(value, &config->view.local_control_socket_mode);
  if (key == CONFIG_CONTROL_GATEWAY_UID) {
    ls200_status status = parse_u32(value, &parsed);
    if (status == LS200_STATUS_OK) config->view.control_gateway_uid = parsed;
    return status;
  }
  return LS200_STATUS_CONFIGURATION_ERROR;
}
static ls200_status apply_limit_key(ls200_config *config, config_key key, const char *value) {
  uint32_t parsed; ls200_status status = parse_u32(value, &parsed);
  if (status != LS200_STATUS_OK) return status;
  switch (key) {
    case CONFIG_LIMITS_SIP_MESSAGE_BYTES: config->view.limits.sip_message_bytes = parsed; break;
    case CONFIG_LIMITS_SDP_BYTES: config->view.limits.sdp_bytes = parsed; break;
    case CONFIG_LIMITS_RTP_PACKET_BYTES: config->view.limits.rtp_packet_bytes = parsed; break;
    case CONFIG_LIMITS_VIDEO_ACCESS_UNIT_BYTES: config->view.limits.video_access_unit_bytes = parsed; break;
    case CONFIG_LIMITS_AUDIO_FRAME_BYTES: config->view.limits.audio_frame_bytes = parsed; break;
    case CONFIG_LIMITS_CONTROL_MESSAGE_BYTES: config->view.limits.control_message_bytes = parsed; break;
    case CONFIG_LIMITS_LOG_EVENTS_PER_INTERVAL: config->view.limits.log_events_per_interval = parsed; break;
    default: return LS200_STATUS_CONFIGURATION_ERROR;
  }
  return LS200_STATUS_OK;
}
ls200_status ls200_config_apply_key(ls200_config *config, config_key key, const char *value) {
  if (config == NULL || value == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (key <= CONFIG_RUNTIME_LOG_SUMMARY_INTERVAL) return apply_runtime_key(config, key, value);
  if (key <= CONFIG_SIP_MAX_RECONNECT_ATTEMPTS) return apply_sip_key(config, key, value);
  if (key <= CONFIG_MEDIA_CHILD_GID) return apply_media_key(config, key, value);
  if (key <= CONFIG_CONTROL_GATEWAY_UID) return apply_control_key(config, key, value);
  if (key < CONFIG_KEY_COUNT) return apply_limit_key(config, key, value);
  return LS200_STATUS_CONFIGURATION_ERROR;
}
