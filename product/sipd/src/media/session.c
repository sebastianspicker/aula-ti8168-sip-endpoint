#include "session_private.h"

ls200_status ls200_media_session_reserve(
    ls200_media_session *session,
    const ls200_sdp_local_capabilities *requested_capabilities,
    ls200_sdp_local_capabilities *out_capabilities) {
  return ls200_media_session_reserve_internal(session, requested_capabilities,
                                              out_capabilities);
}

ls200_status ls200_media_session_prepare(
    ls200_media_session *session,
    const ls200_sdp_negotiated_session *negotiated) {
  return ls200_media_session_prepare_internal(session, negotiated);
}

ls200_status ls200_media_session_commit(ls200_media_session *session) {
  return ls200_media_session_commit_internal(session);
}

ls200_status ls200_media_session_set_video_transmit_enabled(
    ls200_media_session *session, int enabled) {
  return ls200_media_session_set_video_transmit_enabled_internal(session, enabled);
}

ls200_status ls200_media_session_set_audio_muted(ls200_media_session *session,
                                                 int muted) {
  return ls200_media_session_set_audio_muted_internal(session, muted);
}

ls200_status ls200_media_session_request_video_keyframe(
    ls200_media_session *session) {
  return ls200_media_session_request_keyframe_internal(session);
}

void ls200_media_session_rollback(ls200_media_session *session) {
  ls200_media_session_rollback_internal(session);
}

ls200_status ls200_media_session_poll(ls200_media_session *session,
                                      ls200_deadline deadline) {
  return ls200_media_session_poll_internal(session, deadline);
}

ls200_status ls200_media_session_send_dtmf(ls200_media_session *session,
                                           uint8_t digit,
                                           uint16_t duration_samples,
                                           int end) {
  return ls200_media_session_send_dtmf_internal(session, digit,
                                                duration_samples, end);
}

ls200_status ls200_media_session_stop(ls200_media_session *session) {
  return ls200_media_session_stop_internal(session);
}

ls200_status ls200_media_session_get_status(
    const ls200_media_session *session,
    ls200_media_session_status *out_status) {
  return ls200_media_session_get_status_internal(session, out_status);
}

void ls200_media_session_destroy(ls200_media_session *session) {
  ls200_media_session_destroy_internal(session);
}
