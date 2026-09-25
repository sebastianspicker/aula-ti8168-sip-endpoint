#ifndef LS200_SIPD_CONFIG_PRIVATE_H
#define LS200_SIPD_CONFIG_PRIVATE_H

#include "ls200_sipd/config.h"

#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define LS200_CONFIG_LINE_BYTES 1024U
#define LS200_CONFIG_FILE_BYTES 65536U
#define LS200_CONFIG_VALUE_BYTES 512U
#define LS200_CONFIG_PATH_BYTES PATH_MAX
#define LS200_CONFIG_URI_BYTES 512U
#define LS200_CONFIG_BACKEND_BYTES 64U
#define LS200_CONFIG_MEDIA_ADDRESS_BYTES 16U

struct ls200_config {
  ls200_config_view view;
  char pid_file[LS200_CONFIG_PATH_BYTES]; char sip_uri[LS200_CONFIG_URI_BYTES];
  char runtime_settings_file[LS200_CONFIG_PATH_BYTES];
  char auth_username[LS200_CONFIG_VALUE_BYTES]; char auth_secret_file[LS200_CONFIG_PATH_BYTES];
  char sip_profile[32]; char sip_registrar_uri[LS200_CONFIG_URI_BYTES];
  char tls_ca_file[LS200_CONFIG_PATH_BYTES];
  char crc_address[16];
  char media_backend[LS200_CONFIG_BACKEND_BYTES]; char local_control_socket[LS200_CONFIG_PATH_BYTES];
  char video_source[LS200_CONFIG_PATH_BYTES]; char audio_source[LS200_CONFIG_PATH_BYTES];
  char authorized_rtsp_ipv4[LS200_CONFIG_MEDIA_ADDRESS_BYTES];
  char media_bind_address[LS200_CONFIG_MEDIA_ADDRESS_BYTES]; char media_advertised_address[LS200_CONFIG_MEDIA_ADDRESS_BYTES];
  uint32_t transaction_timeout_ms; uint32_t max_retransmissions;
  int fixture_video_fd; int fixture_audio_fd; int use_inherited_fixture_fds;
};
struct ls200_call_config { ls200_config config; };
typedef enum config_key {
  CONFIG_RUNTIME_FOREGROUND, CONFIG_RUNTIME_PID_FILE, CONFIG_RUNTIME_SETTINGS_FILE, CONFIG_RUNTIME_LOG_SINK, CONFIG_RUNTIME_LOG_LEVEL, CONFIG_RUNTIME_LOG_SUMMARY_INTERVAL, CONFIG_SIP_URI, CONFIG_SIP_PROFILE, CONFIG_SIP_REGISTRAR_URI, CONFIG_SIP_TRANSPORT, CONFIG_SIP_AUTH_USERNAME, CONFIG_SIP_AUTH_SECRET_FILE, CONFIG_SIP_TLS_CA_FILE, CONFIG_SIP_CRC_ADDRESS, CONFIG_SIP_ENABLE_TLS, CONFIG_SIP_ENABLE_PUBLIC_NETWORK, CONFIG_SIP_TRANSACTION_TIMEOUT, CONFIG_SIP_MAX_RETRANSMISSIONS, CONFIG_SIP_MAX_RECONNECT_ATTEMPTS, CONFIG_MEDIA_BACKEND, CONFIG_MEDIA_VIDEO_SOURCE, CONFIG_MEDIA_AUDIO_SOURCE, CONFIG_MEDIA_AUTHORIZED_RTSP_IPV4, CONFIG_MEDIA_BIND_ADDRESS, CONFIG_MEDIA_ADVERTISED_ADDRESS, CONFIG_MEDIA_PREFERRED_AUDIO_CODEC, CONFIG_MEDIA_SECURITY, CONFIG_MEDIA_RTSP_USE_UDP, CONFIG_MEDIA_RTSP_JITTER_MAX_MS, CONFIG_MEDIA_RTSP_RECONNECT_LIMIT, CONFIG_MEDIA_LOCAL_RTP_PORT_MIN, CONFIG_MEDIA_LOCAL_RTP_PORT_MAX, CONFIG_MEDIA_RTP_MTU, CONFIG_MEDIA_AEC_ENABLED, CONFIG_MEDIA_AEC_REFERENCE_DELAY_FRAMES, CONFIG_MEDIA_AEC_DELAY_CALIBRATED, CONFIG_MEDIA_VIDEO_QUEUE_FRAMES, CONFIG_MEDIA_AUDIO_QUEUE_FRAMES, CONFIG_MEDIA_CHILD_UID, CONFIG_MEDIA_CHILD_GID, CONFIG_CONTROL_ENABLE_LOCAL_CONTROL, CONFIG_CONTROL_UNIX_SOCKET_PATH, CONFIG_CONTROL_UNIX_SOCKET_MODE, CONFIG_CONTROL_GATEWAY_UID, CONFIG_LIMITS_SIP_MESSAGE_BYTES, CONFIG_LIMITS_SDP_BYTES, CONFIG_LIMITS_RTP_PACKET_BYTES, CONFIG_LIMITS_VIDEO_ACCESS_UNIT_BYTES, CONFIG_LIMITS_AUDIO_FRAME_BYTES, CONFIG_LIMITS_CONTROL_MESSAGE_BYTES, CONFIG_LIMITS_LOG_EVENTS_PER_INTERVAL, CONFIG_KEY_COUNT
} config_key;

void ls200_config_bind_view(ls200_config *config);
void ls200_config_defaults(ls200_config *config);
void ls200_config_close_fixture_fds(ls200_config *config);
ls200_status ls200_config_duplicate_fixture_descriptor(int source, int *out_descriptor);
ls200_status ls200_config_capture_inherited_fixture_fds(ls200_config *config, int required);
int ls200_config_has_control_or_parent_segment(const char *value);
char *ls200_config_trim(char *value);
int ls200_config_parse_ipv4_literal(const char *value, uint8_t octets[4]);
int ls200_config_ipv4_is_loopback(const uint8_t octets[4]);
int ls200_config_ipv4_is_private_or_loopback(const uint8_t octets[4]);
int ls200_config_ipv4_is_authorized_rtsp_target(const uint8_t octets[4]);
ls200_status ls200_config_key_for(const char *section, const char *key, config_key *out_key);
ls200_status ls200_config_apply_key(ls200_config *config, config_key key, const char *value);
ls200_status ls200_config_validate(const ls200_config *config);
ls200_status ls200_config_open_owned_regular_file(const char *path, int secret, int *out_descriptor);
#if defined(LS200_SIPD_TEST_FAULTS)
void ls200_config_test_fail_next_parent_sync(void);
#endif
ls200_status ls200_config_validate_secret_file(const char *path);
/* The optional runtime settings state may be absent on first boot.  If it
 * exists, it is an owner-only regular file under a trusted parent. */
ls200_status ls200_config_validate_runtime_settings_path(const char *path);
ls200_status ls200_config_load_descriptor(int descriptor, int require_unlinked_snapshot, ls200_config **out_config);

#endif
