#ifndef AULA_SIPD_CONTROL_H
#define AULA_SIPD_CONTROL_H

#include "aula_sipd/call.h"
#include "aula_sipd/control_protocol.h"
#include "aula_sipd/media_session.h"
#include "aula_sipd/sip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_control_command {
  AULA_CONTROL_STATUS = 0,
  AULA_CONTROL_HANGUP = 1,
  AULA_CONTROL_SHUTDOWN = 2
} aula_control_command;

/* Command completion is observable by the local client.  In particular, a
 * failed BYE or shutdown transition must never be reported as success. */
typedef aula_status (*aula_control_command_callback)(
    void *context, aula_control_command command);

/* Extended commands are dispatched by opcode. Implementations must validate
 * the opcode-specific JSON schema before changing state. Unsupported request
 * types, including originate until a typed snapshot-replacement API exists,
 * must return AULA_STATUS_UNSUPPORTED. */
typedef aula_status (*aula_control_request_callback)(
    void *context, const aula_control_frame *request,
    aula_mutable_bytes *response_payload);

typedef struct aula_control_config {
  const char *unix_socket_path;
  uint16_t socket_mode;
  uint32_t maximum_request_bytes;
  uint32_t maximum_clients;
  uint32_t authorized_gateway_uid;
  aula_control_command_callback command_callback;
  aula_control_request_callback request_callback;
  void *command_context;
} aula_control_config;

typedef struct aula_endpoint_status {
  aula_call_state call_state;
  aula_zoom_profile sip_profile;
  aula_transport sip_transport;
  aula_sip_registration_status registration;
  /* A snapshot only: status never carries media, endpoints, identifiers, or
   * credentials.  `media_session_present` distinguishes an absent session
   * from the valid NEW state. */
  int media_session_present;
  aula_media_session_state media_session_state;
  aula_media_health video_backend;
  aula_media_health audio_backend;
  aula_receive_shim_stats receive_shim;
  aula_rtp_counters video_rtp;
  aula_rtp_counters audio_rtp;
  uint64_t video_packets_queued;
  uint64_t audio_packets_queued;
  uint64_t video_packets_sent;
  uint64_t audio_packets_sent;
  uint64_t dropped_video_packets;
  uint64_t dropped_audio_packets;
  uint64_t keyframe_requests;
  uint64_t keyframe_request_failures;
  uint64_t rejected_dtmf_requests;
  aula_dtmf_receive_stats inbound_dtmf;
  uint8_t video_payload_type;
  uint8_t audio_payload_type;
  int video_srtp;
  int audio_srtp;
  char active_h264_profile_level_id[7];
  aula_sdp_direction video_direction;
  aula_sdp_direction audio_direction;
  int video_transmit_enabled;
  int audio_muted;
  aula_aec_status aec;
  uint32_t reconnect_attempts;
  aula_status last_error;
  int rx_rendering;
  int rx_audio_rendering;
  int rx_video_rendering;
  int rx_audio_render_fresh;
  int rx_video_render_fresh;
  int rx_renderer_healthy;
  uint64_t rx_audio_last_success_ns;
  uint64_t rx_video_last_success_ns;
} aula_endpoint_status;

typedef struct aula_control_server aula_control_server;

/* Local Unix-domain control only. Network listeners are intentionally absent. */
aula_status aula_control_server_create(const aula_control_config *config,
                                         aula_control_server **out_server);
aula_status aula_control_server_publish_status(aula_control_server *server,
                                                 const aula_endpoint_status *status);
aula_status aula_control_server_poll(aula_control_server *server,
                                       aula_deadline deadline);
void aula_control_server_destroy(aula_control_server *server);

#ifdef __cplusplus
}
#endif

#endif
