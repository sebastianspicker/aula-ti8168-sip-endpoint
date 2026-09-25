#ifndef LS200_SIPD_CONTROL_H
#define LS200_SIPD_CONTROL_H

#include "ls200_sipd/call.h"
#include "ls200_sipd/control_protocol.h"
#include "ls200_sipd/media_session.h"
#include "ls200_sipd/sip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_control_command {
  LS200_CONTROL_STATUS = 0,
  LS200_CONTROL_HANGUP = 1,
  LS200_CONTROL_SHUTDOWN = 2
} ls200_control_command;

/* Command completion is observable by the local client.  In particular, a
 * failed BYE or shutdown transition must never be reported as success. */
typedef ls200_status (*ls200_control_command_callback)(
    void *context, ls200_control_command command);

/* Extended commands are dispatched by opcode. Implementations must validate
 * the opcode-specific JSON schema before changing state. Unsupported request
 * types, including originate until a typed snapshot-replacement API exists,
 * must return LS200_STATUS_UNSUPPORTED. */
typedef ls200_status (*ls200_control_request_callback)(
    void *context, const ls200_control_frame *request,
    ls200_mutable_bytes *response_payload);

typedef struct ls200_control_config {
  const char *unix_socket_path;
  uint16_t socket_mode;
  uint32_t maximum_request_bytes;
  uint32_t maximum_clients;
  uint32_t authorized_gateway_uid;
  ls200_control_command_callback command_callback;
  ls200_control_request_callback request_callback;
  void *command_context;
} ls200_control_config;

typedef struct ls200_endpoint_status {
  ls200_call_state call_state;
  ls200_zoom_profile sip_profile;
  ls200_transport sip_transport;
  ls200_sip_registration_status registration;
  /* A snapshot only: status never carries media, endpoints, identifiers, or
   * credentials.  `media_session_present` distinguishes an absent session
   * from the valid NEW state. */
  int media_session_present;
  ls200_media_session_state media_session_state;
  ls200_media_health video_backend;
  ls200_media_health audio_backend;
  ls200_receive_shim_stats receive_shim;
  ls200_rtp_counters video_rtp;
  ls200_rtp_counters audio_rtp;
  uint64_t video_packets_queued;
  uint64_t audio_packets_queued;
  uint64_t video_packets_sent;
  uint64_t audio_packets_sent;
  uint64_t dropped_video_packets;
  uint64_t dropped_audio_packets;
  uint64_t keyframe_requests;
  uint64_t keyframe_request_failures;
  uint64_t rejected_dtmf_requests;
  ls200_dtmf_receive_stats inbound_dtmf;
  uint8_t video_payload_type;
  uint8_t audio_payload_type;
  int video_srtp;
  int audio_srtp;
  char active_h264_profile_level_id[7];
  ls200_sdp_direction video_direction;
  ls200_sdp_direction audio_direction;
  int video_transmit_enabled;
  int audio_muted;
  ls200_aec_status aec;
  uint32_t reconnect_attempts;
  ls200_status last_error;
  int rx_rendering;
  int rx_audio_rendering;
  int rx_video_rendering;
  int rx_audio_render_fresh;
  int rx_video_render_fresh;
  int rx_renderer_healthy;
  uint64_t rx_audio_last_success_ns;
  uint64_t rx_video_last_success_ns;
} ls200_endpoint_status;

typedef struct ls200_control_server ls200_control_server;

/* Local Unix-domain control only. Network listeners are intentionally absent. */
ls200_status ls200_control_server_create(const ls200_control_config *config,
                                         ls200_control_server **out_server);
ls200_status ls200_control_server_publish_status(ls200_control_server *server,
                                                 const ls200_endpoint_status *status);
ls200_status ls200_control_server_poll(ls200_control_server *server,
                                       ls200_deadline deadline);
void ls200_control_server_destroy(ls200_control_server *server);

#ifdef __cplusplus
}
#endif

#endif
