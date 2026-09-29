#ifndef AULA_SIPD_CONFIG_H
#define AULA_SIPD_CONFIG_H

#include "aula_sipd/log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_transport {
  AULA_TRANSPORT_UDP = 0,
  AULA_TRANSPORT_TCP = 1,
  AULA_TRANSPORT_TLS = 2
} aula_transport;

typedef enum aula_g711_codec {
  AULA_G711_PCMU = 0,
  AULA_G711_PCMA = 1
} aula_g711_codec;

/* Selects the RTP protection profile at the endpoint SDP boundary. */
typedef enum aula_media_security_policy {
  PREFER_SRTP = 0,
  REQUIRE_SRTP,
  PLAIN_COMPAT
} aula_media_security_policy;

typedef struct aula_config_limits {
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
} aula_config_limits;

typedef struct aula_config_view {
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
  aula_transport transport;
  aula_g711_codec preferred_audio_codec;
  aula_media_security_policy media_security_policy;
  aula_log_sink log_sink;
  aula_log_level log_level;
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
  aula_config_limits limits;
} aula_config_view;

typedef struct aula_config_operational_view {
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
} aula_config_operational_view;

/* Fixed local-media identity for a call snapshot.  NAT address translation is
 * intentionally not represented by this configuration boundary. */
typedef struct aula_media_address_options {
  const char *bind_address;
  const char *advertised_address;
} aula_media_address_options;

typedef struct aula_config aula_config;
typedef struct aula_call_config aula_call_config;
struct aula_backend_config;
struct aula_sip_endpoint_config;

/* Rejects unknown keys, duplicate keys, unsafe paths, and invalid limits. */
aula_status aula_config_load_file(const char *path, aula_config **out_config);
/* Linux-only: parses a retained anonymous regular-file descriptor only after
 * verifying WRITE/GROW/SHRINK/SEAL kernel seals. The caller retains ownership
 * of descriptor; other platforms fail closed with UNSUPPORTED. */
aula_status aula_config_load_fd(int descriptor, aula_config **out_config);
const aula_config_view *aula_config_get_view(const aula_config *config);
aula_status aula_config_get_log_config(const aula_config *config,
                                         aula_log_config *out_config);
aula_status aula_config_get_operational_view(const aula_config *config,
                                                aula_config_operational_view *out_view);
aula_status aula_config_create_call_snapshot(const aula_config *config,
                                               aula_call_config **out_snapshot);
const aula_config_view *aula_call_config_get_view(const aula_call_config *snapshot);
aula_status aula_call_config_get_operational_view(
    const aula_call_config *snapshot,
    aula_config_operational_view *out_view);
/* Populate immutable runtime inputs without exposing private config storage. */
aula_status aula_call_config_get_runtime_options(
    const aula_call_config *snapshot,
    struct aula_backend_config *out_backend,
    struct aula_sip_endpoint_config *out_sip,
    aula_media_address_options *out_media_addresses);
void aula_config_destroy(aula_config *config);
void aula_call_config_destroy(aula_call_config *snapshot);

/* Output is safe for diagnostics and must contain no credential material. */
aula_status aula_config_format_redacted(const aula_config *config,
                                          aula_mutable_bytes *output);
/* Reads the already policy-validated, owner-only secret through a fresh
 * no-follow descriptor. The returned bytes exclude one optional line ending;
 * callers must wipe the output immediately after use. */
aula_status aula_config_read_secret_file(const char *path,
                                           aula_mutable_bytes *output);
/* Atomically replaces an existing daemon-owned 0600 secret file.  The parent
 * directory and final entry are revalidated without following symlinks.
 * AULA_STATUS_PERSISTENCE_UNCERTAIN means rename committed the new secret,
 * but parent-directory fsync failed and crash durability is not confirmed. */
aula_status aula_config_replace_secret_file(const char *path,
                                              aula_bytes secret);

#ifdef __cplusplus
}
#endif

#endif
