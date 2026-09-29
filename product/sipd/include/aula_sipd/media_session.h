#ifndef AULA_SIPD_MEDIA_SESSION_H
#define AULA_SIPD_MEDIA_SESSION_H

#include "aula_sipd/backend.h"
#include "aula_sipd/dtmf.h"
#include "aula_sipd/g711.h"
#include "aula_sipd/media_renderer.h"
#include "aula_sipd/media_aec.h"
#include "aula_sipd/rx_shim.h"
#include "aula_sipd/sdp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_MEDIA_SESSION_MAX_INGRESS_PER_POLL 8U
#define AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS UINT64_C(20000000)

typedef enum aula_media_session_state {
  AULA_MEDIA_SESSION_NEW = 0,
  AULA_MEDIA_SESSION_RESERVED,
  AULA_MEDIA_SESSION_PREPARING,
  AULA_MEDIA_SESSION_PREPARED,
  AULA_MEDIA_SESSION_COMMITTED,
  AULA_MEDIA_SESSION_STOPPED
} aula_media_session_state;

/* The backend is moved into the session at create time.  The caller must not
 * call its lifecycle methods afterwards, and must keep backend_config storage
 * alive until the session is destroyed. */
typedef struct aula_media_session_config {
  aula_media_backend backend;
  const aula_backend_config *backend_config;
  aula_rtp_transport_config transport;
  const char *advertised_address;
  const char *local_cname;
  uint16_t rtp_mtu;
  uint32_t maximum_video_queue_packets;
  uint32_t maximum_audio_queue_packets;
  uint32_t maximum_queue_bytes;
  uint32_t maximum_ssrcs;
  uint32_t report_interval_ms;
  int symmetric_rtp_enabled;
  aula_aec_config aec;
  /* Borrowed renderer; the endpoint owns reservation and release. */
  aula_media_renderer *renderer;
} aula_media_session_config;

typedef struct aula_media_session_status {
  aula_media_session_state state;
  aula_media_health backend;
  aula_receive_shim_stats receive;
  aula_rtp_counters video_rtp;
  aula_rtp_counters audio_rtp;
  /* Current depth comes from the bounded pacing queues.  Sent and dropped
   * counters are cumulative for the session lifetime. */
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
  uint64_t last_activity_ns;
  uint8_t video_payload_type;
  uint8_t audio_payload_type;
  /* Derived from the prepared negotiated SDP, never from local preference. */
  int video_srtp;
  int audio_srtp;
  char active_h264_profile_level_id[7];
  aula_sdp_direction video_direction;
  aula_sdp_direction audio_direction;
  /* Runtime gate for the negotiated video send direction. It is false until
   * commit succeeds and remains false when the negotiated direction cannot
   * send video. It never changes the SDP direction. */
  int video_transmit_enabled;
  /* Runtime gate for media-coded outbound audio. RFC 4733 telephone events
   * remain eligible for transmission while this is set. */
  int audio_muted;
  /* Compatibility aggregate: true only while either receive stream has a
   * fresh accepted renderer result. */
  int rx_rendering;
  int rx_audio_rendering;
  int rx_video_rendering;
  int rx_audio_render_fresh;
  int rx_video_render_fresh;
  uint64_t rx_audio_last_success_ns;
  uint64_t rx_video_last_success_ns;
  /* Poll-driven inbound playout accounting.  These counters include bounded
   * drops caused by late/duplicate RTP, renderer backpressure, and underruns. */
  uint64_t rx_audio_frames_rendered;
  uint64_t rx_audio_frames_dropped;
  uint64_t rx_audio_underruns;
  uint64_t rx_video_frames_rendered;
  uint64_t rx_video_frames_dropped;
  uint64_t rx_video_underruns;
  uint64_t rx_reorder_drops;
  uint64_t rx_renderer_failures;
  int rx_renderer_healthy;
  aula_aec_status aec;
  aula_status last_error;
} aula_media_session_status;

typedef struct aula_media_session aula_media_session;

/* Produces an offer-safe capability snapshot.  A requested receive direction
 * is advertised only after reserve, start, and health all succeed for the
 * required local renderer.  Any failure atomically yields the last safe
 * transmit-only or inactive direction; it never advertises sendrecv on a
 * renderer that has not been proven healthy. */
aula_status aula_media_session_gate_renderer(
    aula_media_renderer *renderer,
    const aula_sdp_local_capabilities *requested_capabilities,
    aula_sdp_local_capabilities *out_capabilities);

/* create is local-only: it neither opens the backend nor reserves sockets. */
aula_status aula_media_session_create(const aula_media_session_config *config,
                                        aula_media_session **out_session);
/* Retains actual RTP/RTCP ports before SDP creation.  The input codec arrays
 * remain caller-owned; only the port assignments in out_capabilities change.
 * The caller must use these exact retained assignments in its initial offer. */
aula_status aula_media_session_reserve(
    aula_media_session *session,
    const aula_sdp_local_capabilities *requested_capabilities,
    aula_sdp_local_capabilities *out_capabilities);
/* prepare copies only a fully negotiated answer, validates it against the
 * retained offer ports and payload types, authorizes its RTP and independently
 * addressed non-mux RTCP targets, and transactionally opens the backend and
 * creates bounded packet queues and receive accounting.  It leaves capture
 * stopped. */
aula_status aula_media_session_prepare(
    aula_media_session *session,
    const aula_sdp_negotiated_session *negotiated);
/* Starts the already-prepared backend and receive shim. */
aula_status aula_media_session_commit(aula_media_session *session);
/* Controls only the runtime video transmit gate. This is valid for a
 * committed session whose negotiated video direction permits sending; it is
 * idempotent and does not renegotiate SDP or affect audio/receive media. */
aula_status aula_media_session_set_video_transmit_enabled(
    aula_media_session *session, int enabled);
/* Controls only media-coded outbound audio. It is valid for a committed
 * session, is idempotent, and never suppresses negotiated RFC 4733 DTMF. */
aula_status aula_media_session_set_audio_muted(aula_media_session *session,
                                                 int muted);
/* Requests one backend keyframe through the bounded backend hook. The result
 * is returned unchanged so unsupported or failed backends remain observable. */
aula_status aula_media_session_request_video_keyframe(
    aula_media_session *session);
/* Idempotently releases every prepared resource in reverse order, without
 * emitting media.  It is the failure path before commit. */
void aula_media_session_rollback(aula_media_session *session);
/* Bounded, nonblocking ingress and egress.  At most
 * AULA_MEDIA_SESSION_MAX_INGRESS_PER_POLL datagrams are drained per media
 * transport, and at most one queued RTP packet per transmit stream is sent. */
aula_status aula_media_session_poll(aula_media_session *session,
                                      aula_deadline deadline);
/* Queues one RFC 4733 telephone-event packet on the negotiated audio stream.
 * The caller supplies monotonically non-decreasing duration samples and emits
 * the configured terminal repetitions by repeating the final end request no
 * faster than AULA_MEDIA_SESSION_DTMF_MIN_REQUEST_INTERVAL_NS. */
aula_status aula_media_session_send_dtmf(aula_media_session *session,
                                           uint8_t digit,
                                           uint16_t duration_samples, int end);
/* Emits RTCP BYE on each negotiated transport before reverse-order shutdown.
 * A send failure is reported in status but never prevents cleanup. */
aula_status aula_media_session_stop(aula_media_session *session);
aula_status aula_media_session_get_status(const aula_media_session *session,
                                            aula_media_session_status *out_status);
void aula_media_session_destroy(aula_media_session *session);

#ifdef __cplusplus
}
#endif

#endif
