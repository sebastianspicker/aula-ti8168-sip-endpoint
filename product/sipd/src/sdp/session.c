#include "sdp_internal.h"

#include <stdlib.h>
#include <string.h>

const ls200_sdp_media *ls200_sdp_get_video(const ls200_sdp_session *session) {
  return session != NULL && session->video.present ? &session->video.value : NULL;
}

const ls200_sdp_media *ls200_sdp_get_audio(const ls200_sdp_session *session) {
  return session != NULL && session->audio.present ? &session->audio.value : NULL;
}

void ls200_sdp_session_destroy(ls200_sdp_session *session) {
  if (session != NULL) ls200_sdp_secure_zero(session, sizeof(*session));
  free(session);
}

const ls200_sdp_negotiated_media *ls200_sdp_negotiated_get_video(
    const ls200_sdp_negotiated_session *session) {
  return session == NULL ? NULL : &session->video;
}

const ls200_sdp_negotiated_media *ls200_sdp_negotiated_get_audio(
    const ls200_sdp_negotiated_session *session) {
  return session == NULL ? NULL : &session->audio;
}

const ls200_sdp_receive_media *ls200_sdp_negotiated_get_receive_media(
    const ls200_sdp_negotiated_session *session, int video) {
  if (session == NULL || (video != 0 && video != 1)) return NULL;
  return video != 0 ? &session->receive_video : &session->receive_audio;
}

ls200_status ls200_sdp_negotiated_get_srtp_lifetimes(
    const ls200_sdp_negotiated_session *session, int video,
    ls200_sdp_srtp_lifetimes *out_lifetimes) {
  if (session == NULL || out_lifetimes == NULL || (video != 0 && video != 1))
    return LS200_STATUS_INVALID_ARGUMENT;
  *out_lifetimes = video != 0 ? session->video_srtp_lifetimes :
                                session->audio_srtp_lifetimes;
  return LS200_STATUS_OK;
}

void ls200_sdp_negotiated_session_destroy(ls200_sdp_negotiated_session *session) {
  if (session != NULL) ls200_sdp_secure_zero(session, sizeof(*session));
  free(session);
}
