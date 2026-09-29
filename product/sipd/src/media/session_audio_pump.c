#include "session_private.h"
#include "aula_sipd/platform.h"

#include <string.h>

static aula_status media_session_encode_audio(aula_media_session *session,
                                               aula_bytes pcm, uint32_t sample_rate,
                                               uint8_t channels);

static aula_status media_session_queue_audio_packet(aula_media_session *session) {
  aula_rtp_packet packet;
  uint8_t serialized[12U + AULA_G711_SAMPLES_PER_PACKET];
  aula_mutable_bytes output = {serialized, sizeof(serialized), 0U};
  uint64_t now_ns = 0U;
  aula_status status = aula_g711_packetize_20ms(
      &session->g711_packetizer,
      (aula_bytes){session->audio_remainder, session->audio_remainder_length}, &output);
  if (status == AULA_STATUS_OK) {
    status = aula_rtp_parse((aula_bytes){output.data, output.length}, &packet);
  }
  if (status == AULA_STATUS_OK) {
    packet.header.sequence_number = session->audio_sequence_number++;
    (void)aula_platform_monotonic_now(&now_ns);
    status = aula_rtp_send_queue_enqueue(session->audio.queue, &packet, now_ns);
  }
  session->audio_remainder_length = 0U;
  return status;
}

static aula_status media_session_buffer_encoded_audio(aula_media_session *session,
                                                       aula_bytes encoded) {
  size_t offset = 0U;
  while (offset < encoded.length) {
    size_t available = encoded.length - offset;
    size_t needed = AULA_G711_SAMPLES_PER_PACKET - session->audio_remainder_length;
    size_t copied = available < needed ? available : needed;
    aula_status status;
    (void)memcpy(session->audio_remainder + session->audio_remainder_length,
                 encoded.data + offset, copied);
    session->audio_remainder_length += copied;
    offset += copied;
    if (session->audio_remainder_length != AULA_G711_SAMPLES_PER_PACKET) continue;
    status = media_session_queue_audio_packet(session);
    if (status != AULA_STATUS_OK && status != AULA_STATUS_LIMIT_EXCEEDED) return status;
  }
  return AULA_STATUS_OK;
}

static aula_status media_session_encode_audio(aula_media_session *session,
                                               aula_bytes pcm, uint32_t sample_rate,
                                               uint8_t channels) {
  uint8_t encoded[AULA_SIPD_MAX_AUDIO_FRAME_BYTES / 2U];
  aula_mutable_bytes output = {encoded, sizeof(encoded), 0U};
  aula_status status = aula_media_g711_encode(AULA_MEDIA_AUDIO_PCM_S16LE, pcm,
                                                 sample_rate, channels,
                                                 session->use_pcma, &output);
  if (status != AULA_STATUS_OK) return status;
  return media_session_buffer_encoded_audio(
      session, (aula_bytes){output.data, output.length});
}

static aula_status media_session_process_aec_block(aula_media_session *session) {
  uint8_t aec_pcm[AULA_AEC_FRAME_BYTES];
  aula_mutable_bytes output = {aec_pcm, sizeof(aec_pcm), 0U};
  aula_status status = aula_aec_process_near(
      session->aec, (aula_bytes){session->aec_near_remainder,
                                  session->aec_near_remainder_length}, &output);
  if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) return status;
  return media_session_encode_audio(session, (aula_bytes){output.data, output.length},
                                    AULA_AEC_SAMPLE_RATE, 1U);
}

static aula_status media_session_buffer_aec_near(aula_media_session *session,
                                                  aula_bytes input) {
  size_t offset = 0U;
  while (offset < input.length) {
    size_t available = input.length - offset;
    size_t needed = AULA_AEC_FRAME_BYTES - session->aec_near_remainder_length;
    size_t copied = available < needed ? available : needed;
    aula_status status;
    (void)memcpy(session->aec_near_remainder + session->aec_near_remainder_length,
                 input.data + offset, copied);
    session->aec_near_remainder_length += copied;
    offset += copied;
    if (session->aec_near_remainder_length != AULA_AEC_FRAME_BYTES) continue;
    status = media_session_process_aec_block(session);
    session->aec_near_remainder_length = 0U;
    if (status != AULA_STATUS_OK) return status;
  }
  return AULA_STATUS_OK;
}

static int media_session_aec_input(const aula_media_session *session,
                                   const aula_media_frame *frame) {
  return session->aec != NULL && frame->sample_rate == AULA_AEC_SAMPLE_RATE &&
      frame->channels == 1U;
}

aula_status aula_media_session_pump_audio_internal(aula_media_session *session,
                                                      aula_deadline deadline) {
  aula_media_frame frame;
  aula_status status;
  if (!aula_media_session_direction_sends(session->audio_direction) ||
      session->audio_muted != 0) return AULA_STATUS_AGAIN;
  (void)memset(&frame, 0, sizeof(frame));
  status = session->config.backend.vtable->read_audio_pcm(&session->config.backend, deadline, &frame);
  if (status != AULA_STATUS_OK) return status;
  if (aula_media_validate_frame(&frame) != AULA_STATUS_OK ||
      frame.kind != AULA_MEDIA_AUDIO_PCM_S16LE) return AULA_STATUS_INVALID_DATA;
  if (media_session_aec_input(session, &frame)) {
    return media_session_buffer_aec_near(session, frame.data);
  }
  session->aec_near_remainder_length = 0U;
  return media_session_encode_audio(session, frame.data, frame.sample_rate, frame.channels);
}
