#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "config_private.h"
#include "../sip/crc_route.h"
#include "aula_sipd/backend.h"
#include "aula_sipd/media_aec.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void aula_config_bind_view(aula_config *config) {
  config->view.pid_file = config->pid_file;
  config->view.runtime_settings_file = config->runtime_settings_file;
  config->view.sip_uri = config->sip_uri;
  config->view.auth_username = config->auth_username;
  config->view.auth_secret_file = config->auth_secret_file;
  config->view.sip_profile = config->sip_profile;
  config->view.sip_registrar_uri = config->sip_registrar_uri;
  config->view.tls_ca_file = config->tls_ca_file;
  config->view.media_backend = config->media_backend;
  config->view.video_source = config->video_source;
  config->view.audio_source = config->audio_source;
  config->view.media_bind_address = config->media_bind_address;
  config->view.media_advertised_address = config->media_advertised_address;
  config->view.local_control_socket = config->local_control_socket;
  config->view.sip_transaction_timeout_ms = config->transaction_timeout_ms;
  config->view.sip_max_retransmissions = config->max_retransmissions;
  config->view.sip_max_reconnect_attempts = config->view.limits.reconnect_attempts;
}

void aula_config_defaults(aula_config *config) {
  (void)memset(config, 0, sizeof(*config));
  config->fixture_video_fd = -1;
  config->fixture_audio_fd = -1;
  (void)snprintf(config->pid_file, sizeof(config->pid_file), "%s", "/run/aula-sipd.pid");
  (void)snprintf(config->media_backend, sizeof(config->media_backend), "%s", "fixture");
  (void)snprintf(config->sip_profile, sizeof(config->sip_profile), "%s", "private_lab");
  (void)snprintf(config->authorized_rtsp_ipv4,
                 sizeof(config->authorized_rtsp_ipv4), "%s", "127.0.0.1");
  (void)snprintf(config->media_bind_address, sizeof(config->media_bind_address), "%s",
                 "127.0.0.1");
  (void)snprintf(config->media_advertised_address,
                 sizeof(config->media_advertised_address), "%s", "127.0.0.1");
  (void)snprintf(config->local_control_socket, sizeof(config->local_control_socket), "%s",
                 "/run/aula-sipd/control.sock");
  config->view.transport = AULA_TRANSPORT_UDP;
  config->view.preferred_audio_codec = AULA_G711_PCMU;
  config->view.media_security_policy = PREFER_SRTP;
  config->view.log_sink = AULA_LOG_SINK_STDERR;
  config->view.log_level = AULA_LOG_INFO;
  config->view.local_rtp_port_min = 40000U;
  config->view.local_rtp_port_max = 40100U;
  config->view.media_child_uid = 65534U;
  config->view.media_child_gid = 65534U;
  config->view.control_gateway_uid = 65534U;
  config->view.log_summary_interval_seconds = 60U;
  config->view.rtp_mtu = 1200U;
  config->view.local_control_socket_mode = 0660U;
  config->view.foreground = 1;
  config->view.enable_local_control = 0;
  config->view.enable_public_network = 0;
  config->view.enable_tls = 0;
  config->view.rtsp_use_udp = 0;
  config->view.rtsp_jitter_max_ms = 120U;
  config->view.rtsp_reconnect_limit = 3U;
  config->view.aec_enabled = 0;
  config->view.aec_reference_delay_frames = 0U;
  config->view.aec_delay_calibrated = 0;
  config->view.limits.sip_message_bytes = AULA_SIPD_MAX_SIP_MESSAGE_BYTES;
  config->view.limits.sdp_bytes = AULA_SIPD_MAX_SDP_BYTES;
  config->view.limits.rtp_packet_bytes = AULA_SIPD_MAX_RTP_PACKET_BYTES;
  config->view.limits.video_access_unit_bytes = AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES;
  config->view.limits.audio_frame_bytes = AULA_SIPD_MAX_AUDIO_FRAME_BYTES;
  config->view.limits.control_message_bytes = AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES;
  config->view.limits.video_queue_frames = 8U;
  config->view.limits.audio_queue_frames = 10U;
  config->view.limits.reconnect_attempts = 3U;
  config->view.limits.log_events_per_interval = 120U;
  config->transaction_timeout_ms = 32000U;
  config->max_retransmissions = 6U;
  aula_config_bind_view(config);
}

void aula_config_close_fixture_fds(aula_config *config) {
  if (config == NULL) return;
  if (config->fixture_video_fd >= 0) (void)close(config->fixture_video_fd);
  if (config->fixture_audio_fd >= 0) (void)close(config->fixture_audio_fd);
  config->fixture_video_fd = -1;
  config->fixture_audio_fd = -1;
  config->use_inherited_fixture_fds = 0;
}

aula_status aula_config_duplicate_fixture_descriptor(int source_descriptor,
                                                 int *out_descriptor) {
  struct stat details;
  int duplicate;
  if (source_descriptor < 0 || out_descriptor == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  duplicate = dup(source_descriptor);
  if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
    if (duplicate >= 0) (void)close(duplicate);
    return AULA_STATUS_IO_ERROR;
  }
  if (fstat(duplicate, &details) != 0 || !S_ISREG(details.st_mode)) {
    (void)close(duplicate);
    return AULA_STATUS_PERMISSION_DENIED;
  }
  *out_descriptor = duplicate;
  return AULA_STATUS_OK;
}

static aula_status duplicate_inherited_fixture_fd(const char *value,
                                                   int *out_descriptor) {
  char *end = NULL;
  unsigned long parsed;
  if (value == NULL || value[0] == '\0' || out_descriptor == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > INT_MAX) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  return aula_config_duplicate_fixture_descriptor((int)parsed, out_descriptor);
}

aula_status aula_config_capture_inherited_fixture_fds(
    aula_config *config, int require_unlinked_snapshot) {
  const char *mode = getenv("AULA_SIPD_FIXTURE_FD_MODE");
  const char *video = getenv("AULA_SIPD_FIXTURE_VIDEO_FD");
  const char *audio = getenv("AULA_SIPD_FIXTURE_AUDIO_FD");
  aula_status status;
  if (config == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (mode == NULL && video == NULL && audio == NULL) return AULA_STATUS_OK;
  if (require_unlinked_snapshot == 0 || mode == NULL || video == NULL ||
      audio == NULL || strcmp(mode, "sealed-v1") != 0 ||
      strcmp(config->media_backend, "fixture") != 0) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  status = duplicate_inherited_fixture_fd(video, &config->fixture_video_fd);
  if (status != AULA_STATUS_OK) return status;
  status = duplicate_inherited_fixture_fd(audio, &config->fixture_audio_fd);
  if (status != AULA_STATUS_OK) {
    aula_config_close_fixture_fds(config);
    return status;
  }
  config->use_inherited_fixture_fds = 1;
  return AULA_STATUS_OK;
}

static aula_status validate_regular_file(const char *path) {
  int descriptor;
  aula_status status = aula_config_open_owned_regular_file(path, 0, &descriptor);
  if (status == AULA_STATUS_OK) (void)close(descriptor);
  return status;
}

static aula_status validate_output_path(const char *path) {
  char parent[PATH_MAX];
  char *slash;
  struct stat existing;
  struct stat parent_details;
  if (path == NULL || path[0] != '/' || aula_config_has_control_or_parent_segment(path) ||
      strlen(path) >= sizeof(parent)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  if (lstat(path, &existing) == 0 && S_ISLNK(existing.st_mode)) {
    return AULA_STATUS_SECURITY_ERROR;
  }
  (void)snprintf(parent, sizeof(parent), "%s", path);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent || slash[1] == '\0') {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  *slash = '\0';
  if (lstat(parent, &parent_details) != 0 || !S_ISDIR(parent_details.st_mode) ||
      ((parent_details.st_mode & 0022U) != 0U && (parent_details.st_mode & S_ISVTX) == 0U)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  return AULA_STATUS_OK;
}

static int sip_uri_character_is_valid(unsigned char character) {
  return (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
      (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
      (character >= (unsigned char)'0' && character <= (unsigned char)'9') ||
      strchr("-_.~%+:@;=?", (int)character) != NULL;
}
static int public_sip_uri_is_valid(const char *uri) {
  const char *cursor;
  if (strncmp(uri, "sip:", 4U) != 0 || uri[4] == '\0') return 0;
  for (cursor = uri + 4; *cursor != '\0'; ++cursor) if (!sip_uri_character_is_valid((unsigned char)*cursor)) return 0;
  return 1;
}
static int public_zoom_network_settings_are_valid(const aula_config *config) {
  if (config->view.enable_tls == 0 || config->tls_ca_file[0] == '\0' ||
      config->view.media_security_policy != REQUIRE_SRTP) return 0;
  if (strcmp(config->sip_profile, "zoom_direct") == 0)
    return config->sip_registrar_uri[0] == '\0';
  if (strcmp(config->sip_profile, "zoom_proxy") == 0)
    return public_sip_uri_is_valid(config->sip_registrar_uri) &&
        config->auth_username[0] != '\0' && config->auth_secret_file[0] != '\0';
  return 0;
}
static int profile_media_security_policy_is_valid(const aula_config *config) {
  if (strcmp(config->sip_profile, "private_lab") == 0) return 1;
  return (strcmp(config->sip_profile, "zoom_direct") == 0 ||
          strcmp(config->sip_profile, "zoom_proxy") == 0) &&
      config->view.media_security_policy == REQUIRE_SRTP;
}
static int network_settings_are_valid(const aula_config *config) {
  if ((config->view.transport == AULA_TRANSPORT_TLS) !=
      (config->view.enable_tls != 0) ||
      !profile_media_security_policy_is_valid(config)) return 0;
  if (config->view.enable_public_network) {
    if (config->sip_uri[0] == '\0' || !public_sip_uri_is_valid(config->sip_uri)) return 0;
    if (strcmp(config->sip_profile, "zoom_direct") == 0 ||
        strcmp(config->sip_profile, "zoom_proxy") == 0)
      return public_zoom_network_settings_are_valid(config);
    return strcmp(config->sip_profile, "private_lab") == 0 &&
        config->sip_registrar_uri[0] == '\0';
  }
  return config->sip_uri[0] == '\0';
}
static int media_addresses_are_valid(const aula_config *config) {
  uint8_t bind[4]; uint8_t advertised[4];
  if (!aula_config_parse_ipv4_literal(config->media_bind_address, bind) || !aula_config_parse_ipv4_literal(config->media_advertised_address, advertised) || strcmp(config->media_bind_address, config->media_advertised_address) != 0) return 0;
  return config->view.enable_public_network ? aula_config_ipv4_is_private_or_loopback(bind) && aula_config_ipv4_is_private_or_loopback(advertised) : aula_config_ipv4_is_loopback(bind) && aula_config_ipv4_is_loopback(advertised);
}
static int media_security_policy_is_valid(aula_media_security_policy policy) {
  return policy >= PREFER_SRTP && policy <= PLAIN_COMPAT;
}
static int limit_is_within(uint32_t value, uint32_t minimum, uint32_t maximum) { return value >= minimum && value <= maximum; }
static int packet_limits_are_valid(const aula_config *config) {
  const aula_config_limits *limits = &config->view.limits;
  return limit_is_within(limits->sip_message_bytes, 1U, AULA_SIPD_MAX_SIP_MESSAGE_BYTES) && limit_is_within(limits->sdp_bytes, 1U, AULA_SIPD_MAX_SDP_BYTES) && limit_is_within(limits->rtp_packet_bytes, 256U, AULA_SIPD_MAX_RTP_PACKET_BYTES) && limit_is_within(limits->video_access_unit_bytes, 1U, AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES) && limit_is_within(limits->audio_frame_bytes, 1U, AULA_SIPD_MAX_AUDIO_FRAME_BYTES) && limit_is_within(limits->control_message_bytes, 1U, AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES);
}
static int aec_settings_are_valid(const aula_config *config) {
  return (config->view.aec_enabled == 0 || config->view.aec_enabled == 1) &&
      (config->view.aec_delay_calibrated == 0 || config->view.aec_delay_calibrated == 1) &&
      config->view.aec_reference_delay_frames <= AULA_AEC_MAX_DELAY_FRAMES &&
      (config->view.aec_delay_calibrated == 0 || config->view.aec_enabled != 0);
}
static int runtime_limits_are_valid(const aula_config *config) {
  const aula_config_limits *limits = &config->view.limits;
  return config->view.foreground && config->view.rtp_mtu >= 256U && config->view.rtp_mtu <= limits->rtp_packet_bytes && (config->view.local_rtp_port_min & 1U) == 0U && (config->view.local_rtp_port_max & 1U) == 0U && config->view.local_rtp_port_min <= 65532U && config->view.local_rtp_port_max >= config->view.local_rtp_port_min + 2U && limit_is_within(limits->video_queue_frames, 1U, 128U) && limit_is_within(limits->audio_queue_frames, 1U, 128U) && limit_is_within(limits->log_events_per_interval, 1U, 10000U) && (config->view.local_control_socket_mode == 0600U || config->view.local_control_socket_mode == 0660U) && config->view.control_gateway_uid != UINT32_MAX && aec_settings_are_valid(config);
}
static int backend_settings_are_valid(const aula_config *config) {
  uint8_t authorized_rtsp[4];
  if (!aula_config_parse_ipv4_literal(config->authorized_rtsp_ipv4, authorized_rtsp) ||
      !aula_config_ipv4_is_authorized_rtsp_target(authorized_rtsp)) return 0;
  if (strcmp(config->media_backend, "rtsp_gst_process") == 0 ||
      strcmp(config->media_backend, "rtsp_native") == 0) {
    if (config->view.media_child_uid == 0U || config->view.media_child_gid == 0U ||
        config->view.media_child_uid == UINT32_MAX || config->view.media_child_gid == UINT32_MAX) return 0;
    if (aula_rtsp_authorized_uri_validate(config->video_source,
                                           config->authorized_rtsp_ipv4) != AULA_STATUS_OK) return 0;
  }
  return config->use_inherited_fixture_fds == 0 || (strcmp(config->media_backend, "fixture") == 0 && config->fixture_video_fd >= 0 && config->fixture_audio_fd >= 0);
}

static int crc_route_settings_are_valid(const aula_config *config) {
  if (config->crc_address[0] == '\0') return 1;
  return strcmp(config->sip_profile, "zoom_direct") == 0 &&
      config->view.enable_public_network != 0 &&
      config->view.transport == AULA_TRANSPORT_TLS &&
      config->view.enable_tls != 0 &&
      config->view.media_security_policy == REQUIRE_SRTP &&
      aula_sip_crc_address_is_valid(config->crc_address);
}

static int structural_settings_are_valid(const aula_config *config) {
  return network_settings_are_valid(config) && crc_route_settings_are_valid(config) &&
         media_addresses_are_valid(config) &&
         media_security_policy_is_valid(config->view.media_security_policy) &&
         packet_limits_are_valid(config) && runtime_limits_are_valid(config) &&
         backend_settings_are_valid(config);
}

static aula_status validate_runtime_paths(const aula_config *config) {
  aula_status status = validate_output_path(config->pid_file);
  if (status != AULA_STATUS_OK) return status;
  status = aula_config_validate_runtime_settings_path(
      config->runtime_settings_file);
  if (status != AULA_STATUS_OK || !config->view.enable_local_control) {
    return status;
  }
  return validate_output_path(config->local_control_socket);
}

static aula_status validate_credential_files(const aula_config *config) {
  aula_status status;
  if (config->auth_secret_file[0] != '\0') {
    status = aula_config_validate_secret_file(config->auth_secret_file);
    if (status != AULA_STATUS_OK) return status;
  }
  if (config->tls_ca_file[0] != '\0') {
    return validate_regular_file(config->tls_ca_file);
  }
  return AULA_STATUS_OK;
}

aula_status aula_config_validate(const aula_config *config) {
  aula_status status;
  if (config == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (!structural_settings_are_valid(config)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  status = validate_runtime_paths(config);
  return status == AULA_STATUS_OK ? validate_credential_files(config) : status;
}
