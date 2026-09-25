#include "session_private.h"
#include "ls200_sipd/platform.h"

#include <string.h>

static ls200_status media_session_encode_audio(ls200_media_session *session,
                                               ls200_bytes pcm, uint32_t sample_rate,
                                               uint8_t channels);

static ls200_status media_session_queue_audio_packet(ls200_media_session *session) {
  ls200_rtp_packet packet;
  uint8_t serialized[12U + LS200_G711_SAMPLES_PER_PACKET];
  ls200_mutable_bytes output = {serialized, sizeof(serialized), 0U};
  uint64_t now_ns = 0U;
  ls200_status status = ls200_g711_packetize_20ms(
      &session->g711_packetizer,
      (ls200_bytes){session->audio_remainder, session->audio_remainder_length}, &output);
  if (status == LS200_STATUS_OK) {
    status = ls200_rtp_parse((ls200_bytes){output.data, output.length}, &packet);
  }
  if (status == LS200_STATUS_OK) {
    packet.header.sequence_number = session->audio_sequence_number++;
    (void)ls200_platform_monotonic_now(&now_ns);
    status = ls200_rtp_send_queue_enqueue(session->audio.queue, &packet, now_ns);
  }
  session->audio_remainder_length = 0U;
  return status;
}

static ls200_status media_session_buffer_encoded_audio(ls200_media_session *session,
                                                       ls200_bytes encoded) {
  size_t offset = 0U;
  while (offset < encoded.length) {
    size_t available = encoded.length - offset;
    size_t needed = LS200_G711_SAMPLES_PER_PACKET - session->audio_remainder_length;
    size_t copied = available < needed ? available : needed;
    ls200_status status;
    (void)memcpy(session->audio_remainder + session->audio_remainder_length,
                 encoded.data + offset, copied);
    session->audio_remainder_length += copied;
    offset += copied;
    if (session->audio_remainder_length != LS200_G711_SAMPLES_PER_PACKET) continue;
    status = media_session_queue_audio_packet(session);
    if (status != LS200_STATUS_OK && status != LS200_STATUS_LIMIT_EXCEEDED) return status;
  }
  return LS200_STATUS_OK;
}

static ls200_status media_session_encode_audio(ls200_media_session *session,
                                               ls200_bytes pcm, uint32_t sample_rate,
                                               uint8_t channels) {
  uint8_t encoded[LS200_SIPD_MAX_AUDIO_FRAME_BYTES / 2U];
  ls200_mutable_bytes output = {encoded, sizeof(encoded), 0U};
  ls200_status status = ls200_media_g711_encode(LS200_MEDIA_AUDIO_PCM_S16LE, pcm,
                                                 sample_rate, channels,
                                                 session->use_pcma, &output);
  if (status != LS200_STATUS_OK) return status;
  return media_session_buffer_encoded_audio(
      session, (ls200_bytes){output.data, output.length});
}

static ls200_status media_session_process_aec_block(ls200_media_session *session) {
  uint8_t aec_pcm[LS200_AEC_FRAME_BYTES];
  ls200_mutable_bytes output = {aec_pcm, sizeof(aec_pcm), 0U};
  ls200_status status = ls200_aec_process_near(
      session->aec, (ls200_bytes){session->aec_near_remainder,
                                  session->aec_near_remainder_length}, &output);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  return media_session_encode_audio(session, (ls200_bytes){output.data, output.length},
                                    LS200_AEC_SAMPLE_RATE, 1U);
}

static ls200_status media_session_buffer_aec_near(ls200_media_session *session,
                                                  ls200_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    size_t available = input.length - offset;
    size_t needed = LS200_AEC_FRAME_BYTES - session->aec_near_remainder_length;
    size_t copied = available < needed ? available : needed;
    ls200_status status;
    (void)memcpy(session->aec_near_remainder + session->aec_near_remainder_length,
                 input.data + offset, copied);
    session->aec_near_remainder_length += copied;
    offset += copied;
    if (session->aec_near_remainder_length != LS200_AEC_FRAME_BYTES) continue;
    status = media_session_process_aec_block(session);
    session->aec_near_remainder_length = 0U;
    if (status != LS200_STATUS_OK) return status;
  }
  return LS200_STATUS_OK;
}

static int media_session_aec_input(const ls200_media_session *session,
                                   const ls200_media_frame *frame) {
  return session->aec != NULL && frame->sample_rate == LS200_AEC_SAMPLE_RATE &&
      frame->channels == 1U;
}

ls200_status ls200_media_session_pump_audio_internal(ls200_media_session *session,
                                                      ls200_deadline deadline) {
  ls200_media_frame frame;
  ls200_status status;
  if (!ls200_media_session_direction_sends(session->audio_direction) ||
      session->audio_muted != 0) return LS200_STATUS_AGAIN;
  (void)memset(&frame, 0, sizeof(frame));
  status = session->config.backend.vtable->read_audio_pcm(&session->config.backend, deadline, &frame);
  if (status != LS200_STATUS_OK) return status;
  if (ls200_media_validate_frame(&frame) != LS200_STATUS_OK ||
      frame.kind != LS200_MEDIA_AUDIO_PCM_S16LE) return LS200_STATUS_INVALID_DATA;
  if (media_session_aec_input(session, &frame)) {
    return media_session_buffer_aec_near(session, frame.data);
  }
  session->aec_near_remainder_length = 0U;
  return media_session_encode_audio(session, frame.data, frame.sample_rate, frame.channels);
}
