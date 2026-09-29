#ifndef AULA_SIPD_BACKEND_H
#define AULA_SIPD_BACKEND_H

#include "aula_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_backend_config {
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
} aula_backend_config;

typedef struct aula_media_backend aula_media_backend;

typedef enum aula_rtsp_message_kind {
  AULA_RTSP_MESSAGE_RESPONSE = 0,
  AULA_RTSP_MESSAGE_INTERLEAVED_RTP = 1
} aula_rtsp_message_kind;

typedef struct aula_rtsp_message {
  aula_rtsp_message_kind kind;
  uint16_t status_code;
  uint32_t cseq;
  uint8_t channel;
  char session_id[128];
  aula_bytes body;
} aula_rtsp_message;

typedef struct aula_rtsp_stream_parser aula_rtsp_stream_parser;
typedef struct aula_rtsp_protocol_fixture aula_rtsp_protocol_fixture;

typedef struct aula_media_backend_vtable {
  aula_status (*open)(aula_media_backend *backend,
                       const aula_backend_config *config);
  aula_status (*start)(aula_media_backend *backend);
  aula_status (*read_video_access_unit)(aula_media_backend *backend,
                                         aula_deadline deadline,
                                         aula_media_frame *out_frame);
  aula_status (*read_audio_pcm)(aula_media_backend *backend,
                                 aula_deadline deadline,
                                 aula_media_frame *out_frame);
  aula_status (*request_video_keyframe)(aula_media_backend *backend);
  aula_status (*health)(const aula_media_backend *backend,
                         aula_media_health *out_health);
  aula_status (*stop)(aula_media_backend *backend);
  void (*close)(aula_media_backend *backend);
} aula_media_backend_vtable;

struct aula_media_backend {
  const aula_media_backend_vtable *vtable;
  void *context;
};

/* Public backend contracts contain no CBox, GStreamer, SysLink, DSP, or M3 types. */
aula_status aula_media_backend_validate(const aula_media_backend *backend);
/* Deterministic fixture backend for host and ARM self-tests. */
aula_status aula_fixture_backend_create(aula_media_backend *out_backend);
/* Stable FNV-1a fingerprint of fixture bytes emitted so far, including stream
 * separators.  It lets tests assert repeatability without inspecting files. */
aula_status aula_fixture_backend_fingerprint(const aula_media_backend *backend,
                                               uint64_t *out_fingerprint);
/* Accepts only rtsp://127.0.0.1[:port]/safe-path URIs; no DNS or credentials. */
aula_status aula_rtsp_local_uri_validate(const char *uri);
/* Accepts only a lowercase rtsp URI for the exact canonical, permitted IPv4
 * target.  The target and URI never undergo name resolution. */
aula_status aula_rtsp_authorized_uri_validate(const char *uri,
                                                const char *authorized_ipv4);
/* A message body remains valid until the next parser push. */
aula_status aula_rtsp_stream_parser_create(aula_rtsp_stream_parser **out_parser);
aula_status aula_rtsp_stream_parser_push(aula_rtsp_stream_parser *parser,
                                           aula_bytes input,
                                           aula_rtsp_message *out_message);
void aula_rtsp_stream_parser_destroy(aula_rtsp_stream_parser *parser);
/* In-memory RTSP protocol fixture.  It never opens a socket or starts the
 * PCM child; callers inject complete or fragmented RTSP stream bytes and
 * inspect the next emitted request or completed H.264 access unit. */
aula_status aula_rtsp_protocol_fixture_create(uint32_t maximum_access_unit_bytes,
                                                aula_rtsp_protocol_fixture **out_fixture);
aula_status aula_rtsp_protocol_fixture_start(aula_rtsp_protocol_fixture *fixture,
                                               aula_mutable_bytes *out_request);
aula_status aula_rtsp_protocol_fixture_push(aula_rtsp_protocol_fixture *fixture,
                                              aula_bytes input,
                                              aula_mutable_bytes *out_request,
                                              aula_media_frame *out_frame);
aula_status aula_rtsp_protocol_fixture_eof(aula_rtsp_protocol_fixture *fixture);
void aula_rtsp_protocol_fixture_destroy(aula_rtsp_protocol_fixture *fixture);
/* Native dual-track RTSP (/movie/track1 H264, /movie/track2 MPEG4-GENERIC
 * AAC).  Loopback acquisition is opt-in only when
 * AULA_SIPD_ENABLE_LOCAL_RTSP is exactly 1.  A non-loopback authorized
 * target requires AULA_SIPD_ENABLE_AUTHORIZED_RTSP. */
aula_status aula_rtsp_native_backend_create(aula_media_backend *out_backend);
/* Deprecated compatibility spelling.  It creates the native backend and
 * never starts GStreamer, ALSA, or a media child process. */
aula_status aula_rtsp_gst_process_backend_create(aula_media_backend *out_backend);

#ifdef __cplusplus
}
#endif

#endif
