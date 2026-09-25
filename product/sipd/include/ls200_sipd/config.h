#ifndef LS200_SIPD_CONFIG_H
#define LS200_SIPD_CONFIG_H

#include "ls200_sipd/log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_transport {
  LS200_TRANSPORT_UDP = 0,
  LS200_TRANSPORT_TCP = 1,
  LS200_TRANSPORT_TLS = 2
} ls200_transport;

typedef enum ls200_g711_codec {
  LS200_G711_PCMU = 0,
  LS200_G711_PCMA = 1
} ls200_g711_codec;

/* Selects the RTP protection profile at the endpoint SDP boundary. */
typedef enum ls200_media_security_policy {
  PREFER_SRTP = 0,
  REQUIRE_SRTP,
  PLAIN_COMPAT
} ls200_media_security_policy;

typedef struct ls200_config_limits {
  uint32_t sip_message_bytes;
  uint32_t sdp_bytes;
  uint32_t rtp_packet_bytes;
  uint32_t video_access_unit_bytes;
  uint32_t audio_frame_bytes;
  uint32_t control_message_bytes;
  uint32_t video_queue_frames;
  uint32_t audio_queue_frames;
  uint32_t reconnect_attempts;
  uint32_t log_events_per_interval;
} ls200_config_limits;

typedef struct ls200_config_view {
  const char *pid_file;
  /* Optional daemon-owned LSZ1 settings state.  When configured it must be
   * an absolute path under a non-group/world-writable directory. */
  const char *runtime_settings_file;
  const char *sip_uri;
  const char *auth_username;
  const char *auth_secret_file;
  const char *sip_profile;
  const char *sip_registrar_uri;
  const char *tls_ca_file;
  const char *media_backend;
  const char *video_source;
  const char *audio_source;
  const char *media_bind_address;
  const char *media_advertised_address;
  const char *local_control_socket;
  ls200_transport transport;
  ls200_g711_codec preferred_audio_codec;
  ls200_media_security_policy media_security_policy;
  ls200_log_sink log_sink;
  ls200_log_level log_level;
  uint16_t local_rtp_port_min;
  uint16_t local_rtp_port_max;
  uint32_t media_child_uid;
  uint32_t media_child_gid;
  uint32_t control_gateway_uid;
  uint32_t log_summary_interval_seconds;
  uint32_t sip_transaction_timeout_ms;
  uint32_t sip_max_retransmissions;
  uint32_t sip_max_reconnect_attempts;
  uint32_t rtsp_jitter_max_ms;
  uint32_t rtsp_reconnect_limit;
  uint32_t aec_reference_delay_frames;
  uint16_t rtp_mtu;
  uint16_t local_control_socket_mode;
  int foreground;
  int enable_local_control;
  int enable_public_network;
  int enable_tls;
  int rtsp_use_udp;
  int aec_enabled;
  int aec_delay_calibrated;
  ls200_config_limits limits;
} ls200_config_view;

typedef struct ls200_config_operational_view {
  const char *pid_file;
  const char *media_backend;
  const char *video_source;
  const char *audio_source;
  const char *media_bind_address;
  const char *media_advertised_address;
  const char *local_control_socket;
  uint16_t local_control_socket_mode;
  uint16_t rtp_mtu;
  uint16_t local_rtp_port_min;
  uint16_t local_rtp_port_max;
  uint32_t media_child_uid;
  uint32_t media_child_gid;
  uint32_t control_gateway_uid;
  uint32_t log_summary_interval_seconds;
  uint32_t sip_transaction_timeout_ms;
  uint32_t sip_max_retransmissions;
  uint32_t sip_max_reconnect_attempts;
  uint32_t rtsp_jitter_max_ms;
  uint32_t rtsp_reconnect_limit;
  uint32_t aec_reference_delay_frames;
  int foreground;
  int enable_local_control;
  int enable_public_network;
  int enable_tls;
  int rtsp_use_udp;
  int aec_enabled;
  int aec_delay_calibrated;
} ls200_config_operational_view;

/* Fixed local-media identity for a call snapshot.  NAT address translation is
 * intentionally not represented by this configuration boundary. */
typedef struct ls200_media_address_options {
  const char *bind_address;
  const char *advertised_address;
} ls200_media_address_options;

typedef struct ls200_config ls200_config;
typedef struct ls200_call_config ls200_call_config;
struct ls200_backend_config;
struct ls200_sip_endpoint_config;

/* Rejects unknown keys, duplicate keys, unsafe paths, and invalid limits. */
ls200_status ls200_config_load_file(const char *path, ls200_config **out_config);
/* Linux-only: parses a retained anonymous regular-file descriptor only after
 * verifying WRITE/GROW/SHRINK/SEAL kernel seals. The caller retains ownership
 * of descriptor; other platforms fail closed with UNSUPPORTED. */
ls200_status ls200_config_load_fd(int descriptor, ls200_config **out_config);
const ls200_config_view *ls200_config_get_view(const ls200_config *config);
ls200_status ls200_config_get_log_config(const ls200_config *config,
                                         ls200_log_config *out_config);
ls200_status ls200_config_get_operational_view(const ls200_config *config,
                                                ls200_config_operational_view *out_view);
ls200_status ls200_config_create_call_snapshot(const ls200_config *config,
                                               ls200_call_config **out_snapshot);
const ls200_config_view *ls200_call_config_get_view(const ls200_call_config *snapshot);
ls200_status ls200_call_config_get_operational_view(
    const ls200_call_config *snapshot,
    ls200_config_operational_view *out_view);
/* Populate immutable runtime inputs without exposing private config storage. */
ls200_status ls200_call_config_get_runtime_options(
    const ls200_call_config *snapshot,
    struct ls200_backend_config *out_backend,
    struct ls200_sip_endpoint_config *out_sip,
    ls200_media_address_options *out_media_addresses);
void ls200_config_destroy(ls200_config *config);
void ls200_call_config_destroy(ls200_call_config *snapshot);

/* Output is safe for diagnostics and must contain no credential material. */
ls200_status ls200_config_format_redacted(const ls200_config *config,
                                          ls200_mutable_bytes *output);
/* Reads the already policy-validated, owner-only secret through a fresh
 * no-follow descriptor. The returned bytes exclude one optional line ending;
 * callers must wipe the output immediately after use. */
ls200_status ls200_config_read_secret_file(const char *path,
                                           ls200_mutable_bytes *output);
/* Atomically replaces an existing daemon-owned 0600 secret file.  The parent
 * directory and final entry are revalidated without following symlinks.
 * LS200_STATUS_PERSISTENCE_UNCERTAIN means rename committed the new secret,
 * but parent-directory fsync failed and crash durability is not confirmed. */
ls200_status ls200_config_replace_secret_file(const char *path,
                                              ls200_bytes secret);

#ifdef __cplusplus
}
#endif

#endif
