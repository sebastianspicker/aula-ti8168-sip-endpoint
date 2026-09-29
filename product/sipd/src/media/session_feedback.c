#include "session_private.h"
#include "../rtcp/feedback.h"
#include "../rtp/retransmit_private.h"
#include "aula_sipd/platform.h"

typedef struct media_feedback_context {
  aula_media_session *session;
  uint32_t sender_ssrc;
  uint64_t now_ns;
} media_feedback_context;

aula_status aula_media_session_prepare_feedback_internal(
    aula_media_session *session, const aula_sdp_negotiated_session *negotiated) {
  session->video_feedback_mask = aula_sdp_negotiated_video_feedback(negotiated);
  session->last_video_feedback_ns = 0U;
  session->video_feedback_requests = 0U;
  session->video_retransmitted_packets = 0U;
  session->requested_video_bitrate_bps = 0U;
  session->have_fir_sequence = 0;
  return aula_rtp_retransmit_enable(session->video.transport,
      (session->video_feedback_mask & AULA_SDP_FEEDBACK_NACK) != 0U);
}

static void media_feedback_refresh(const aula_rtcp_feedback_event *event,
                                   media_feedback_context *context) {
  aula_media_session *session = context->session;
  uint32_t required = event->kind == AULA_RTCP_FEEDBACK_PLI ?
      AULA_SDP_FEEDBACK_PLI : AULA_SDP_FEEDBACK_FIR;
  if ((session->video_feedback_mask & required) == 0U) return;
  if (event->kind == AULA_RTCP_FEEDBACK_FIR) {
    if (session->have_fir_sequence != 0 && session->last_fir_sender == event->sender_ssrc &&
        session->last_fir_sequence == event->fir_sequence) return;
    session->last_fir_sender = event->sender_ssrc;
    session->last_fir_sequence = event->fir_sequence;
    session->have_fir_sequence = 1;
  }
  if (session->last_video_feedback_ns != 0U &&
      (context->now_ns < session->last_video_feedback_ns ||
       context->now_ns - session->last_video_feedback_ns <
           AULA_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS * UINT64_C(1000000))) return;
  session->last_video_feedback_ns = context->now_ns;
  session->video_feedback_requests++;
  (void)aula_media_session_request_keyframe_internal(session);
}

static void media_feedback_visit(const aula_rtcp_feedback_event *event, void *opaque) {
  media_feedback_context *context = opaque;
  aula_media_session *session = context->session;
  /* Authentication and approved tuple checks precede this callback. A compound
   * cannot use its first sender to authorize feedback from another SSRC. */
  if (event->sender_ssrc != context->sender_ssrc ||
      event->media_ssrc != session->video.identity.ssrc) {
    session->rejected_packets++;
    return;
  }
  switch (event->kind) {
    case AULA_RTCP_FEEDBACK_NACK:
      if ((session->video_feedback_mask & AULA_SDP_FEEDBACK_NACK) != 0U)
        (void)aula_rtp_retransmit_request(session->video.transport,
            event->media_ssrc, event->nack_pid, event->nack_blp);
      break;
    case AULA_RTCP_FEEDBACK_PLI:
    case AULA_RTCP_FEEDBACK_FIR:
      media_feedback_refresh(event, context);
      break;
    case AULA_RTCP_FEEDBACK_TMMBR:
      /* The OEM preset interface restarts capture. Until a live encoder control
       * is available, retain the request without advertising support or sending
       * a TMMBN that would falsely claim the cap was applied. */
      session->requested_video_bitrate_bps = event->bitrate_bps;
      break;
  }
}

aula_status aula_media_session_accept_feedback_internal(
    aula_media_session *session, aula_media_session_stream *stream,
    uint32_t sender_ssrc, aula_bytes packet) {
  media_feedback_context context = {session, sender_ssrc, 0U};
  aula_status status;
  if (stream != &session->video) return aula_rtcp_visit_feedback(packet, NULL, NULL);
  status = aula_platform_monotonic_now(&context.now_ns);
  if (status != AULA_STATUS_OK) return status;
  return aula_rtcp_visit_feedback(packet, media_feedback_visit, &context);
}

aula_status aula_media_session_flush_repairs_internal(aula_media_session *session) {
  aula_rtp_retransmit_delta delta;
  uint64_t now_ns = 0U;
  aula_status status;
  if (session->video_transmit_enabled == 0 ||
      (session->video_feedback_mask & AULA_SDP_FEEDBACK_NACK) == 0U)
    return AULA_STATUS_OK;
  status = aula_platform_monotonic_now(&now_ns);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_retransmit_drain(session->video.transport, now_ns, &delta);
  session->video.sent_packets += delta.packets;
  session->video.sent_octets += delta.payload_bytes;
  session->video_retransmitted_packets += delta.packets;
  return status;
}
