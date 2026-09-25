#include "session_private.h"

void ls200_media_session_prepare_finalize_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio) {
  session->video_direction = video->local_direction; session->audio_direction = audio->local_direction;
  session->h264_packetizer.mtu = session->config.rtp_mtu; session->h264_packetizer.payload_type = session->video_payload_type;
  session->h264_packetizer.ssrc = session->video.identity.ssrc; session->h264_packetizer.sequence_number = session->video.identity.initial_sequence_number;
  session->h264_packetizer.timestamp = session->video.identity.initial_timestamp;
  session->g711_packetizer.codec = session->use_pcma != 0 ? LS200_G711_PCMA : LS200_G711_PCMU;
  session->g711_packetizer.payload_type = session->audio_payload_type; session->g711_packetizer.ssrc = session->audio.identity.ssrc;
  session->g711_packetizer.sequence_number = session->audio.identity.initial_sequence_number;
  session->g711_packetizer.rtp_timestamp = session->audio.identity.initial_timestamp;
  session->audio_sequence_number = session->audio.identity.initial_sequence_number; session->state = LS200_MEDIA_SESSION_PREPARED;
  session->status.state = session->state; session->status.video_direction = session->video_direction;
  session->status.audio_direction = session->audio_direction;
  session->status.video_payload_type = session->video_payload_type; session->status.audio_payload_type = session->audio_payload_type;
  session->status.video_srtp = video->srtp_enabled != 0;
  session->status.audio_srtp = audio->srtp_enabled != 0;
}
