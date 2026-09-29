#include "sdp_internal.h"

#include <stdlib.h>
#include <string.h>

const aula_sdp_media *aula_sdp_get_video(const aula_sdp_session *session) {
  return session != NULL && session->video.present ? &session->video.value : NULL;
}

const aula_sdp_media *aula_sdp_get_audio(const aula_sdp_session *session) {
  return session != NULL && session->audio.present ? &session->audio.value : NULL;
}

void aula_sdp_session_destroy(aula_sdp_session *session) {
  if (session != NULL) aula_sdp_secure_zero(session, sizeof(*session));
  free(session);
}

const aula_sdp_negotiated_media *aula_sdp_negotiated_get_video(
    const aula_sdp_negotiated_session *session) {
  return session == NULL ? NULL : &session->video;
}

const aula_sdp_negotiated_media *aula_sdp_negotiated_get_audio(
    const aula_sdp_negotiated_session *session) {
  return session == NULL ? NULL : &session->audio;
}

const aula_sdp_receive_media *aula_sdp_negotiated_get_receive_media(
    const aula_sdp_negotiated_session *session, int video) {
  if (session == NULL || (video != 0 && video != 1)) return NULL;
  return video != 0 ? &session->receive_video : &session->receive_audio;
}

aula_status aula_sdp_negotiated_get_srtp_lifetimes(
    const aula_sdp_negotiated_session *session, int video,
    aula_sdp_srtp_lifetimes *out_lifetimes) {
  if (session == NULL || out_lifetimes == NULL || (video != 0 && video != 1))
    return AULA_STATUS_INVALID_ARGUMENT;
  *out_lifetimes = video != 0 ? session->video_srtp_lifetimes :
                                session->audio_srtp_lifetimes;
  return AULA_STATUS_OK;
}

void aula_sdp_negotiated_session_destroy(aula_sdp_negotiated_session *session) {
  if (session != NULL) aula_sdp_secure_zero(session, sizeof(*session));
  free(session);
}
