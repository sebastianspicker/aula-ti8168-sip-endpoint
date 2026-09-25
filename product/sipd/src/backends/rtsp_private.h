#ifndef LS200_RTSP_PRIVATE_H
#define LS200_RTSP_PRIVATE_H

#include "ls200_sipd/backend.h"
#include "ls200_sipd/h264.h"
#include "ls200_sipd/media_aac.h"
#include "ls200_sipd/platform.h"

#define LS200_GST_MAX_RESTARTS 3U
#define LS200_RTSP_MAX_HEADER_BYTES 8192U
#define LS200_RTSP_MAX_BODY_BYTES 8192U
#define LS200_RTSP_STREAM_BUFFER_BYTES \
  (LS200_RTSP_MAX_HEADER_BYTES + LS200_RTSP_MAX_BODY_BYTES + 4U)
#define LS200_RTSP_NATIVE_DEFAULT_JITTER_MS 120U
#define LS200_RTSP_NATIVE_MAX_JITTER_MS 2000U
#define LS200_RTSP_NATIVE_DEFAULT_RECONNECTS 3U
#define LS200_RTSP_NATIVE_RECONNECT_BASE_MS 100U
#define LS200_RTSP_NATIVE_RECONNECT_MAX_MS 1000U
#define LS200_RTSP_NATIVE_MAX_REORDER_PACKETS 32U
#define LS200_RTSP_NATIVE_MAX_UDP_OPEN_ATTEMPTS 256U
#define LS200_RTSP_NATIVE_MAX_UDP_DATAGRAMS_PER_CHANNEL 8U
#define LS200_RTSP_NATIVE_DEFAULT_SESSION_TIMEOUT_SECONDS 30U
#define LS200_RTSP_NATIVE_MAX_KEEPALIVE_INTERVAL_SECONDS 10U
#define LS200_RTSP_NATIVE_KEEPALIVE_RESPONSE_SECONDS 5U

typedef enum ls200_rtsp_session_state {
  LS200_RTSP_DISABLED = 0, LS200_RTSP_CONNECTING, LS200_RTSP_OPTIONS,
  LS200_RTSP_DESCRIBE, LS200_RTSP_SETUP, LS200_RTSP_PLAY,
  LS200_RTSP_STREAMING, LS200_RTSP_FAILED
} ls200_rtsp_session_state;

typedef struct ls200_rtsp_uri {
  char text[320];
  uint8_t address[4];
  uint16_t port;
} ls200_rtsp_uri;

typedef struct ls200_rtsp_h264_capability {
  char profile_level_id[7];
} ls200_rtsp_h264_capability;

/* Parsed SDP selection for the native dual-track backend.  The values point
 * nowhere into the SDP body, so they remain valid after the stream parser
 * receives the next response. */
typedef struct ls200_rtsp_native_tracks {
  ls200_rtsp_uri video_uri;
  ls200_rtsp_uri audio_uri;
  ls200_aac_config aac;
  ls200_rtsp_h264_capability h264;
  uint8_t h264_payload_type;
  uint8_t aac_payload_type;
} ls200_rtsp_native_tracks;

typedef struct native_reorder_packet {
  uint8_t data[LS200_SIPD_MAX_RTP_PACKET_BYTES];
  size_t length;
  uint16_t sequence;
  int occupied;
} native_reorder_packet;

typedef struct native_reorder_state {
  native_reorder_packet packets[LS200_RTSP_NATIVE_MAX_REORDER_PACKETS];
  uint16_t expected_sequence;
  uint64_t gap_started_ns;
  int expected_valid;
} native_reorder_state;

typedef enum native_state {
  NATIVE_CLOSED = 0, NATIVE_CONNECTING, NATIVE_OPTIONS, NATIVE_DESCRIBE,
  NATIVE_SETUP_VIDEO, NATIVE_SETUP_AUDIO, NATIVE_PLAY, NATIVE_STREAMING
} native_state;

typedef struct native_context {
  ls200_rtsp_uri aggregate_uri;
  ls200_rtsp_uri video_uri;
  ls200_rtsp_uri audio_uri;
  ls200_rtsp_stream_parser *parser;
  ls200_h264_depacketizer *depacketizer;
  ls200_aac_decoder *aac_decoder;
  ls200_h264_parameter_sets parameter_sets;
  ls200_aac_config aac;
  uint8_t *assembly;
  uint8_t *video;
  uint8_t pcm[LS200_SIPD_MAX_AUDIO_FRAME_BYTES];
  size_t assembly_length;
  size_t video_length;
  size_t pcm_length;
  int video_ready;
  int audio_ready;
  int fd;
  int video_udp_fd;
  int video_rtcp_fd;
  int audio_udp_fd;
  int audio_rtcp_fd;
  int opened;
  int started;
  int use_udp;
  int discovery_only;
  int discovery_complete;
  int expected_h264_present;
  uint16_t cseq;
  uint32_t session_timeout_seconds;
  uint32_t keepalive_cseq;
  uint8_t h264_payload_type;
  uint8_t aac_payload_type;
  ls200_rtsp_h264_capability discovered_h264;
  ls200_rtsp_h264_capability expected_h264;
  uint32_t maximum_access_unit_bytes;
  uint32_t maximum_pcm_frame_bytes;
  uint32_t jitter_max_ms;
  uint32_t reconnect_limit;
  uint32_t reconnect_attempts;
  uint64_t reconnect_after_ns;
  uint64_t keepalive_due_ns;
  uint64_t keepalive_deadline_ns;
  uint64_t video_pts_ns;
  uint64_t audio_pts_ns;
  char session[128];
  char pending[1024];
  size_t pending_length;
  size_t pending_offset;
  uint16_t video_source_port;
  uint16_t video_rtcp_source_port;
  uint16_t audio_source_port;
  uint16_t audio_rtcp_source_port;
  int video_keyframe;
  int video_synchronized;
  int keepalive_pending;
  native_reorder_state video_reorder;
  native_reorder_state audio_reorder;
  native_state state;
  native_state pending_state;
  ls200_media_health health;
} native_context;

struct ls200_rtsp_stream_parser {
  uint8_t buffer[LS200_RTSP_STREAM_BUFFER_BYTES];
  size_t length;
  size_t consumed;
  int message_active;
  uint32_t session_timeout_seconds;
  int session_timeout_present;
};

typedef struct ls200_rtsp_gst_context {
  char device_property[320];
  ls200_rtsp_uri video_uri;
  ls200_rtsp_uri setup_uri;
  int audio_read_fd;
  int rtsp_fd;
  ls200_child_process child;
  ls200_rtsp_stream_parser *parser;
  ls200_h264_depacketizer *depacketizer;
  ls200_h264_parameter_sets parameter_sets;
  uint8_t pcm[320];
  size_t pcm_length;
  uint8_t *assembly;
  uint8_t *video;
  size_t assembly_length;
  size_t video_length;
  uint64_t audio_pts_ns;
  uint64_t video_pts_ns;
  uint32_t maximum_access_unit_bytes;
  uint32_t maximum_pcm_frame_bytes;
  uint32_t media_child_uid;
  uint32_t media_child_gid;
  uint32_t rtsp_cseq;
  uint8_t h264_payload_type;
  char pending_request[1024];
  size_t pending_request_length;
  size_t pending_request_offset;
  char fixture_request[1024];
  size_t fixture_request_length;
  ls200_rtsp_session_state pending_state;
  ls200_rtsp_session_state rtsp_state;
  int video_ready;
  int protocol_fixture;
  int opened;
  int started;
  ls200_media_health health;
} ls200_rtsp_gst_context;

struct ls200_rtsp_protocol_fixture { ls200_rtsp_gst_context context; };

ls200_status ls200_rtsp_drive_request(ls200_rtsp_gst_context *context);
ls200_status ls200_rtsp_flush_request(ls200_rtsp_gst_context *context);
ls200_status ls200_rtsp_consume_message(ls200_rtsp_gst_context *context,
                                        const ls200_rtsp_message *message);
int ls200_rtsp_path_character(unsigned char character);
int ls200_rtsp_ipv4_literal_matches(const char *value, const uint8_t address[4]);
int ls200_rtsp_ipv4_is_loopback(const uint8_t address[4]);
ls200_status ls200_rtsp_parse_authorized_uri(const char *uri,
                                             const char *authorized_ipv4,
                                             ls200_rtsp_uri *out_uri);
ls200_status ls200_rtsp_parse_local_uri(const char *uri, ls200_rtsp_uri *out_uri);
ls200_status ls200_rtsp_sdp_select_native_tracks(
    const ls200_rtsp_uri *aggregate_uri, ls200_bytes sdp,
    ls200_rtsp_native_tracks *out_tracks);
ls200_status ls200_rtsp_native_discover_h264(
    const ls200_backend_config *config, ls200_deadline deadline,
    ls200_rtsp_h264_capability *out_capability);
ls200_status ls200_rtsp_native_backend_expect_h264(
    ls200_media_backend *backend,
    const ls200_rtsp_h264_capability *capability);
ls200_status ls200_rtsp_aac_au_from_payload(ls200_bytes payload,
                                             ls200_bytes *out_access_unit);
int ls200_rtsp_native_enabled(const native_context *context);
ls200_status ls200_rtsp_native_send(native_context *context, const char *method,
                                    const char *uri, const char *headers,
                                    native_state next_state);
ls200_status ls200_rtsp_native_flush(native_context *context);
ls200_status ls200_rtsp_native_connect(native_context *context);
ls200_status ls200_rtsp_native_start_request(native_context *context);
ls200_status ls200_rtsp_native_transport_header(native_context *context,
                                                 int *rtp_fd, int *rtcp_fd,
                                                 unsigned int first_channel,
                                                 char *headers, size_t capacity);
ls200_status ls200_rtsp_native_drain_udp(native_context *context);
void ls200_rtsp_native_close_transports(native_context *context);
ls200_status ls200_rtsp_native_push_video(native_context *context,
                                          ls200_bytes bytes);
ls200_status ls200_rtsp_native_push_audio(native_context *context,
                                          ls200_bytes bytes);
ls200_status ls200_rtsp_native_expire_reorder(native_context *context);
void ls200_rtsp_native_clear_reorder(native_reorder_state *state);
ls200_status ls200_rtsp_native_reset_stream(native_context *context);
ls200_status ls200_rtsp_native_schedule_reconnect(native_context *context,
                                                   ls200_status cause);
ls200_status ls200_rtsp_native_prepare_io(native_context *context);
void ls200_rtsp_native_arm_keepalive(native_context *context, uint64_t now_ns);
int ls200_rtsp_stream_parser_session_timeout(
    const ls200_rtsp_stream_parser *parser, uint32_t *out_seconds);
/* Native backend-only deterministic seam.  It is intentionally private to
 * unit tests and permits loopback transport tests without an AAC dependency. */
ls200_status ls200_rtsp_native_test_set_stream(ls200_media_backend *backend,
                                               uint8_t h264_payload_type,
                                               uint8_t aac_payload_type);
ls200_status ls200_rtsp_native_test_dispatch_tcp(ls200_media_backend *backend,
                                                  uint8_t channel,
                                                  ls200_bytes packet);
ls200_status ls200_rtsp_native_test_open_udp(ls200_media_backend *backend);
ls200_status ls200_rtsp_native_test_udp_port(const ls200_media_backend *backend,
                                              int video, int rtcp,
                                              uint16_t *out_port);
ls200_status ls200_rtsp_native_test_drain_udp(ls200_media_backend *backend);
ls200_status ls200_rtsp_native_test_dispatch_udp(ls200_media_backend *backend,
                                                  int video, int rtcp,
                                                  uint16_t source_port,
                                                  ls200_bytes packet);
ls200_status ls200_rtsp_native_test_fail(ls200_media_backend *backend,
                                         ls200_status cause);
ls200_status ls200_rtsp_native_test_pump(ls200_media_backend *backend);
#if defined(LS200_SIPD_TEST_FAULTS) && LS200_SIPD_TEST_FAULTS
ls200_status ls200_rtsp_native_test_set_discovered_h264(
    const char profile_level_id[7]);
#endif

#endif
