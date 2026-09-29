#define _POSIX_C_SOURCE 200809L
#include "config_private.h"
#include "config_route.h"
#include "aula_sipd/backend.h"
#include "aula_sipd/sip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *aula_config_crc_address(const aula_config *config) {
  return config == NULL ? NULL : config->crc_address;
}

const aula_config_view *aula_config_get_view(const aula_config *config) { return (config == NULL) ? NULL : &config->view; }

aula_status aula_config_get_log_config(const aula_config *config,
                                         aula_log_config *out_config) {
  if (config == NULL || out_config == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  out_config->sink = config->view.log_sink;
  out_config->minimum_level = config->view.log_level;
  out_config->maximum_events_per_interval = config->view.limits.log_events_per_interval;
  out_config->interval_seconds = config->view.log_summary_interval_seconds;
  return AULA_STATUS_OK;
}

static void aula_config_copy_operational_view(const aula_config_view *view,
                                         aula_config_operational_view *out_view) {
  out_view->pid_file = view->pid_file;
  out_view->media_backend = view->media_backend;
  out_view->video_source = view->video_source;
  out_view->audio_source = view->audio_source;
  out_view->media_bind_address = view->media_bind_address;
  out_view->media_advertised_address = view->media_advertised_address;
  out_view->local_control_socket = view->local_control_socket;
  out_view->local_control_socket_mode = view->local_control_socket_mode;
  out_view->control_gateway_uid = view->control_gateway_uid;
  out_view->rtp_mtu = view->rtp_mtu;
  out_view->local_rtp_port_min = view->local_rtp_port_min;
  out_view->local_rtp_port_max = view->local_rtp_port_max;
  out_view->media_child_uid = view->media_child_uid;
  out_view->media_child_gid = view->media_child_gid;
  out_view->log_summary_interval_seconds = view->log_summary_interval_seconds;
  out_view->sip_transaction_timeout_ms = view->sip_transaction_timeout_ms;
  out_view->sip_max_retransmissions = view->sip_max_retransmissions;
  out_view->sip_max_reconnect_attempts = view->sip_max_reconnect_attempts;
  out_view->rtsp_jitter_max_ms = view->rtsp_jitter_max_ms;
  out_view->rtsp_reconnect_limit = view->rtsp_reconnect_limit;
  out_view->aec_reference_delay_frames = view->aec_reference_delay_frames;
  out_view->foreground = view->foreground;
  out_view->enable_local_control = view->enable_local_control;
  out_view->enable_public_network = view->enable_public_network;
  out_view->enable_tls = view->enable_tls;
  out_view->rtsp_use_udp = view->rtsp_use_udp;
  out_view->aec_enabled = view->aec_enabled;
  out_view->aec_delay_calibrated = view->aec_delay_calibrated;
}

static aula_status aula_config_sip_profile(const char *value,
                                             aula_zoom_profile *out_profile) {
  if (value == NULL || out_profile == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (strcmp(value, "zoom_direct") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_DIRECT_CRC;
  } else if (strcmp(value, "zoom_proxy") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_PROXY_REGISTRATION;
  } else if (strcmp(value, "private_lab") == 0) {
    *out_profile = AULA_ZOOM_PROFILE_PRIVATE_LAB;
  } else {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  return AULA_STATUS_OK;
}

static const char *aula_config_media_security_name(
    aula_media_security_policy policy) {
  if (policy == PREFER_SRTP) return "prefer_srtp";
  if (policy == REQUIRE_SRTP) return "required";
  return "plain_compat";
}

aula_status aula_config_get_operational_view(const aula_config *config,
                                                aula_config_operational_view *out_view) {
  if (config == NULL || out_view == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  aula_config_copy_operational_view(&config->view, out_view);
  return AULA_STATUS_OK;
}

aula_status aula_config_create_call_snapshot(const aula_config *config, aula_call_config **out_snapshot) {
  aula_call_config *snapshot;
  if (config == NULL || out_snapshot == NULL || *out_snapshot != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  snapshot = (aula_call_config *)calloc(1U, sizeof(*snapshot));
  if (snapshot == NULL) return AULA_STATUS_INTERNAL_ERROR;
  snapshot->config = *config;
  snapshot->config.fixture_video_fd = -1;
  snapshot->config.fixture_audio_fd = -1;
  if (config->use_inherited_fixture_fds != 0) {
    aula_status status = aula_config_duplicate_fixture_descriptor(
        config->fixture_video_fd, &snapshot->config.fixture_video_fd);
    if (status == AULA_STATUS_OK) {
      status = aula_config_duplicate_fixture_descriptor(config->fixture_audio_fd,
                                             &snapshot->config.fixture_audio_fd);
    }
    if (status != AULA_STATUS_OK) {
      aula_config_close_fixture_fds(&snapshot->config);
      (void)memset(snapshot, 0, sizeof(*snapshot));
      free(snapshot);
      return status;
    }
  }
  aula_config_bind_view(&snapshot->config);
  *out_snapshot = snapshot;
  return AULA_STATUS_OK;
}

const aula_config_view *aula_call_config_get_view(const aula_call_config *snapshot) { return (snapshot == NULL) ? NULL : &snapshot->config.view; }

aula_status aula_call_config_get_operational_view(
    const aula_call_config *snapshot, aula_config_operational_view *out_view) {
  if (snapshot == NULL || out_view == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  aula_config_copy_operational_view(&snapshot->config.view, out_view);
  return AULA_STATUS_OK;
}

aula_status aula_call_config_get_runtime_options(
    const aula_call_config *snapshot, aula_backend_config *out_backend,
    aula_sip_endpoint_config *out_sip,
    aula_media_address_options *out_media_addresses) {
  const aula_config_view *view;
  if (snapshot == NULL || out_backend == NULL || out_sip == NULL ||
      out_media_addresses == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  view = &snapshot->config.view;
  (void)memset(out_backend, 0, sizeof(*out_backend));
  out_backend->name = view->media_backend;
  out_backend->video_source = view->video_source;
  out_backend->audio_source = view->audio_source;
  out_backend->authorized_rtsp_ipv4 = snapshot->config.authorized_rtsp_ipv4;
  out_backend->use_inherited_fixture_fds = snapshot->config.use_inherited_fixture_fds;
  out_backend->fixture_video_fd = snapshot->config.fixture_video_fd;
  out_backend->fixture_audio_fd = snapshot->config.fixture_audio_fd;
  out_backend->maximum_access_unit_bytes = view->limits.video_access_unit_bytes;
  out_backend->maximum_pcm_frame_bytes = view->limits.audio_frame_bytes;
  out_backend->media_child_uid = view->media_child_uid;
  out_backend->media_child_gid = view->media_child_gid;
  out_backend->rtsp_use_udp = view->rtsp_use_udp;
  out_backend->rtsp_jitter_max_ms = view->rtsp_jitter_max_ms;
  out_backend->rtsp_reconnect_limit = view->rtsp_reconnect_limit;
  (void)memset(out_sip, 0, sizeof(*out_sip));
  out_sip->outbound_uri = view->sip_uri;
  if (aula_config_sip_profile(view->sip_profile, &out_sip->profile) !=
      AULA_STATUS_OK)
    return AULA_STATUS_CONFIGURATION_ERROR;
  out_sip->auth_username = view->auth_username[0] == '\0' ? NULL :
                                                            view->auth_username;
  out_sip->auth_secret_file = view->auth_secret_file[0] == '\0' ? NULL :
                                                                  view->auth_secret_file;
  out_sip->transaction_timeout_ms = view->sip_transaction_timeout_ms;
  out_sip->max_retransmissions = view->sip_max_retransmissions;
  out_sip->max_reconnect_attempts = view->sip_max_reconnect_attempts;
  out_sip->max_digest_retries = 2U;
  out_sip->limits.maximum_message_bytes = view->limits.sip_message_bytes;
  out_sip->limits.maximum_sdp_bytes = view->limits.sdp_bytes;
  out_sip->limits.maximum_header_count = 128U;
  out_sip->limits.maximum_transaction_count = 16U;
  out_sip->limits.maximum_dialog_count = 2U;
  out_sip->limits.maximum_digest_challenges_per_dialog = 2U;
  out_sip->allow_tcp_fallback = view->transport == AULA_TRANSPORT_UDP;
  out_sip->enable_tls = view->enable_tls;
  out_sip->preferred_transport = view->transport;
  out_media_addresses->bind_address = view->media_bind_address;
  out_media_addresses->advertised_address = view->media_advertised_address;
  return AULA_STATUS_OK;
}

void aula_config_destroy(aula_config *config) {
  if (config != NULL) {
    aula_config_close_fixture_fds(config);
    (void)memset(config, 0, sizeof(*config));
    free(config);
  }
}
void aula_call_config_destroy(aula_call_config *snapshot) {
  if (snapshot != NULL) {
    aula_config_close_fixture_fds(&snapshot->config);
    (void)memset(snapshot, 0, sizeof(*snapshot));
    free(snapshot);
  }
}

aula_status aula_config_format_redacted(const aula_config *config, aula_mutable_bytes *output) {
  int written;
  if (config == NULL || output == NULL || output->data == NULL || output->capacity == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  written = snprintf((char *)output->data, output->capacity,
                     "foreground=%s transport=%s backend=%s security=%s local_control=%s public_network=%s tls=%s aec=%s aec_delay_frames=%u aec_calibrated=%s rx_rendering=false",
                     config->view.foreground ? "true" : "false",
                     config->view.transport == AULA_TRANSPORT_UDP ? "udp" : (config->view.transport == AULA_TRANSPORT_TCP ? "tcp" : "tls"),
                     config->media_backend,
                     aula_config_media_security_name(config->view.media_security_policy),
                     config->view.enable_local_control ? "enabled" : "disabled",
                     config->view.enable_public_network ? "enabled" : "disabled", config->view.enable_tls ? "enabled" : "disabled",
                     config->view.aec_enabled ? "enabled" : "disabled",
                     (unsigned int)config->view.aec_reference_delay_frames,
                     config->view.aec_delay_calibrated ? "true" : "false");
  if (written < 0 || (size_t)written >= output->capacity) { output->length = 0U; return AULA_STATUS_LIMIT_EXCEEDED; }
  output->length = (size_t)written;
  return AULA_STATUS_OK;
}
