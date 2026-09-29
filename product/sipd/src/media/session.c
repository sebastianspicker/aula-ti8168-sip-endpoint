#include "session_private.h"

aula_status aula_media_session_reserve(
    aula_media_session *session,
    const aula_sdp_local_capabilities *requested_capabilities,
    aula_sdp_local_capabilities *out_capabilities) {
  return aula_media_session_reserve_internal(session, requested_capabilities,
                                              out_capabilities);
}

aula_status aula_media_session_prepare(
    aula_media_session *session,
    const aula_sdp_negotiated_session *negotiated) {
  return aula_media_session_prepare_internal(session, negotiated);
}

aula_status aula_media_session_commit(aula_media_session *session) {
  return aula_media_session_commit_internal(session);
}

aula_status aula_media_session_set_video_transmit_enabled(
    aula_media_session *session, int enabled) {
  return aula_media_session_set_video_transmit_enabled_internal(session, enabled);
}

aula_status aula_media_session_set_audio_muted(aula_media_session *session,
                                                 int muted) {
  return aula_media_session_set_audio_muted_internal(session, muted);
}

aula_status aula_media_session_request_video_keyframe(
    aula_media_session *session) {
  return aula_media_session_request_keyframe_internal(session);
}

void aula_media_session_rollback(aula_media_session *session) {
  aula_media_session_rollback_internal(session);
}

aula_status aula_media_session_poll(aula_media_session *session,
                                      aula_deadline deadline) {
  return aula_media_session_poll_internal(session, deadline);
}

aula_status aula_media_session_send_dtmf(aula_media_session *session,
                                           uint8_t digit,
                                           uint16_t duration_samples,
                                           int end) {
  return aula_media_session_send_dtmf_internal(session, digit,
                                                duration_samples, end);
}

aula_status aula_media_session_stop(aula_media_session *session) {
  return aula_media_session_stop_internal(session);
}

aula_status aula_media_session_get_status(
    const aula_media_session *session,
    aula_media_session_status *out_status) {
  return aula_media_session_get_status_internal(session, out_status);
}

void aula_media_session_destroy(aula_media_session *session) {
  aula_media_session_destroy_internal(session);
}
