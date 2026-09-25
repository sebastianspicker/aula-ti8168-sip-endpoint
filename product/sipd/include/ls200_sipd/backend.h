#ifndef LS200_SIPD_BACKEND_H
#define LS200_SIPD_BACKEND_H

#include "ls200_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_backend_config {
  const char *name;
  const char *video_source;
  const char *audio_source;
  /* Canonical IPv4 target permitted for RTSP ingest.  This is operational
   * data: callers must not log or otherwise publish it. */
  const char *authorized_rtsp_ipv4;
  /* Optional retained fixture inputs.  When use_inherited_fixture_fds is 1,
   * the fixture backend duplicates these descriptors and must not reopen
   * either source pathname.  Leave the flag at zero for ordinary explicit
   * file-path tests. */
  int use_inherited_fixture_fds;
  int fixture_video_fd;
  int fixture_audio_fd;
  uint32_t maximum_access_unit_bytes;
  uint32_t maximum_pcm_frame_bytes;
  /* Exact unprivileged identity for a process-backed media child. */
  uint32_t media_child_uid;
  uint32_t media_child_gid;
  /* Native RTSP transport policy.  Zero retains TCP interleaving, which is
   * the safest default on a NATed appliance.  Nonzero selects bounded UDP
   * RTP/RTCP sockets supplied by the backend. */
  int rtsp_use_udp;
  uint32_t rtsp_jitter_max_ms;
  uint32_t rtsp_reconnect_limit;
} ls200_backend_config;

typedef struct ls200_media_backend ls200_media_backend;

typedef enum ls200_rtsp_message_kind {
  LS200_RTSP_MESSAGE_RESPONSE = 0,
  LS200_RTSP_MESSAGE_INTERLEAVED_RTP = 1
} ls200_rtsp_message_kind;

typedef struct ls200_rtsp_message {
  ls200_rtsp_message_kind kind;
  uint16_t status_code;
  uint32_t cseq;
  uint8_t channel;
  char session_id[128];
  ls200_bytes body;
} ls200_rtsp_message;

typedef struct ls200_rtsp_stream_parser ls200_rtsp_stream_parser;
typedef struct ls200_rtsp_protocol_fixture ls200_rtsp_protocol_fixture;

typedef struct ls200_media_backend_vtable {
  ls200_status (*open)(ls200_media_backend *backend,
                       const ls200_backend_config *config);
  ls200_status (*start)(ls200_media_backend *backend);
  ls200_status (*read_video_access_unit)(ls200_media_backend *backend,
                                         ls200_deadline deadline,
                                         ls200_media_frame *out_frame);
  ls200_status (*read_audio_pcm)(ls200_media_backend *backend,
                                 ls200_deadline deadline,
                                 ls200_media_frame *out_frame);
  ls200_status (*request_video_keyframe)(ls200_media_backend *backend);
  ls200_status (*health)(const ls200_media_backend *backend,
                         ls200_media_health *out_health);
  ls200_status (*stop)(ls200_media_backend *backend);
  void (*close)(ls200_media_backend *backend);
} ls200_media_backend_vtable;

struct ls200_media_backend {
  const ls200_media_backend_vtable *vtable;
  void *context;
};

/* Public backend contracts contain no CBox, GStreamer, SysLink, DSP, or M3 types. */
ls200_status ls200_media_backend_validate(const ls200_media_backend *backend);
/* Deterministic fixture backend for host and ARM self-tests. */
ls200_status ls200_fixture_backend_create(ls200_media_backend *out_backend);
/* Stable FNV-1a fingerprint of fixture bytes emitted so far, including stream
 * separators.  It lets tests assert repeatability without inspecting files. */
ls200_status ls200_fixture_backend_fingerprint(const ls200_media_backend *backend,
                                               uint64_t *out_fingerprint);
/* Accepts only rtsp://127.0.0.1[:port]/safe-path URIs; no DNS or credentials. */
ls200_status ls200_rtsp_local_uri_validate(const char *uri);
/* Accepts only a lowercase rtsp URI for the exact canonical, permitted IPv4
 * target.  The target and URI never undergo name resolution. */
ls200_status ls200_rtsp_authorized_uri_validate(const char *uri,
                                                const char *authorized_ipv4);
/* A message body remains valid until the next parser push. */
ls200_status ls200_rtsp_stream_parser_create(ls200_rtsp_stream_parser **out_parser);
ls200_status ls200_rtsp_stream_parser_push(ls200_rtsp_stream_parser *parser,
                                           ls200_bytes input,
                                           ls200_rtsp_message *out_message);
void ls200_rtsp_stream_parser_destroy(ls200_rtsp_stream_parser *parser);
/* In-memory RTSP protocol fixture.  It never opens a socket or starts the
 * PCM child; callers inject complete or fragmented RTSP stream bytes and
 * inspect the next emitted request or completed H.264 access unit. */
ls200_status ls200_rtsp_protocol_fixture_create(uint32_t maximum_access_unit_bytes,
                                                ls200_rtsp_protocol_fixture **out_fixture);
ls200_status ls200_rtsp_protocol_fixture_start(ls200_rtsp_protocol_fixture *fixture,
                                               ls200_mutable_bytes *out_request);
ls200_status ls200_rtsp_protocol_fixture_push(ls200_rtsp_protocol_fixture *fixture,
                                              ls200_bytes input,
                                              ls200_mutable_bytes *out_request,
                                              ls200_media_frame *out_frame);
ls200_status ls200_rtsp_protocol_fixture_eof(ls200_rtsp_protocol_fixture *fixture);
void ls200_rtsp_protocol_fixture_destroy(ls200_rtsp_protocol_fixture *fixture);
/* Native dual-track RTSP (/movie/track1 H264, /movie/track2 MPEG4-GENERIC
 * AAC).  Loopback acquisition is opt-in only when
 * LS200_SIPD_ENABLE_LOCAL_RTSP is exactly 1.  A non-loopback authorized
 * target requires LS200_SIPD_ENABLE_AUTHORIZED_RTSP. */
ls200_status ls200_rtsp_native_backend_create(ls200_media_backend *out_backend);
/* Deprecated compatibility spelling.  It creates the native backend and
 * never starts GStreamer, ALSA, or a media child process. */
ls200_status ls200_rtsp_gst_process_backend_create(ls200_media_backend *out_backend);

#ifdef __cplusplus
}
#endif

#endif
