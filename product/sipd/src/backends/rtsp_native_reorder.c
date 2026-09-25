#include "rtsp_private.h"
#include "ls200_sipd/media_aac.h"
#include "ls200_sipd/rtp.h"
#include "../media/h264_profile_private.h"

#include <string.h>

typedef ls200_status (*native_rtp_handler)(native_context *context,
                                           ls200_bytes bytes);

static ls200_status native_observe_h264_nal(
    native_context *context, const ls200_h264_nal *nal,
    char discovered_profile[7], int *idr) {
  char profile_level_id[7];
  if (ls200_h264_parameter_sets_update(&context->parameter_sets, nal) !=
      LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  if (nal->type == 7U) {
    if (ls200_h264_profile_level_id(&context->parameter_sets,
                                    profile_level_id) != LS200_STATUS_OK ||
        !ls200_h264_profile_level_id_valid(profile_level_id))
      return LS200_STATUS_INVALID_DATA;
    if (context->expected_h264_present != 0 &&
        !ls200_h264_profile_level_id_compatible(
            context->expected_h264.profile_level_id, profile_level_id))
      return LS200_STATUS_INVALID_DATA;
    if (context->discovery_only != 0)
      (void)memcpy(discovered_profile, profile_level_id, 7U);
  }
  if (nal->is_idr != 0) *idr = 1;
  return LS200_STATUS_OK;
}

static ls200_status native_complete_h264_discovery(
    native_context *context, const char discovered_profile[7]) {
  if (context->discovery_only == 0 || discovered_profile[0] == '\0')
    return LS200_STATUS_AGAIN;
  (void)memcpy(context->discovered_h264.profile_level_id,
               discovered_profile, 7U);
  context->discovery_complete = 1;
  context->assembly_length = 0U;
  return LS200_STATUS_OK;
}

static ls200_status native_reset_video_assembly(native_context *context) {
  ls200_status status;
  ls200_h264_depacketizer_destroy(context->depacketizer);
  context->depacketizer = NULL;
  context->assembly_length = 0U;
  context->video_synchronized = 0;
  status = ls200_h264_depacketizer_create(context->maximum_access_unit_bytes,
                                           &context->depacketizer);
  return status;
}

static int native_h264_fu_start(const ls200_rtp_packet *packet) {
  return packet->payload.length >= 3U &&
      (packet->payload.data[0] & 0x80U) == 0U &&
      (packet->payload.data[0] & 0x1fU) == 28U &&
      (packet->payload.data[1] & 0x80U) != 0U &&
      (packet->payload.data[1] & 0x1fU) != 0U;
}

static int native_h264_orphan_fragment(const ls200_rtp_packet *packet) {
  int end;
  if (packet->payload.length < 3U ||
      (packet->payload.data[0] & 0x80U) != 0U ||
      (packet->payload.data[0] & 0x1fU) != 28U ||
      (packet->payload.data[1] & 0x80U) != 0U ||
      (packet->payload.data[1] & 0x1fU) == 0U) return 0;
  end = (packet->payload.data[1] & 0x40U) != 0U;
  return end == (packet->header.marker != 0U);
}

static ls200_status native_finish_video(native_context *context,
                                        uint32_t timestamp) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  ls200_status status;
  size_t required;
  uint32_t parameter_generation = context->parameter_sets.generation;
  char discovered_profile[7] = {0};
  int idr = 0;
  if (context->assembly_length == 0U || ls200_h264_annexb_iterator_init(
          &iterator, (ls200_bytes){context->assembly, context->assembly_length},
          context->maximum_access_unit_bytes) != LS200_STATUS_OK) {
    return LS200_STATUS_INVALID_DATA;
  }
  for (;;) {
    status = ls200_h264_annexb_iterator_next(&iterator, &nal);
    if (status == LS200_STATUS_END) break;
    if (status != LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
    status = native_observe_h264_nal(context, &nal, discovered_profile, &idr);
    if (context->parameter_sets.generation != parameter_generation) {
      context->video_synchronized = 0;
    }
    if (status != LS200_STATUS_OK) return LS200_STATUS_INVALID_DATA;
  }
  status = native_complete_h264_discovery(context, discovered_profile);
  if (status == LS200_STATUS_OK) return status;
  if ((idr != 0 && ls200_h264_can_transmit_access_unit(
          &context->parameter_sets, 1) == 0) ||
      (idr == 0 && context->video_synchronized == 0)) {
    context->assembly_length = 0U;
    context->health.frames_dropped++;
    return LS200_STATUS_AGAIN;
  }
  if (idr != 0) context->video_synchronized = 1;
  required = 8U + context->parameter_sets.sps_length +
      context->parameter_sets.pps_length + context->assembly_length;
  if (required > context->maximum_access_unit_bytes) return LS200_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(context->video, "\x00\x00\x00\x01", 4U);
  (void)memcpy(context->video + 4U, context->parameter_sets.sps,
               context->parameter_sets.sps_length);
  (void)memcpy(context->video + 4U + context->parameter_sets.sps_length,
               "\x00\x00\x00\x01", 4U);
  (void)memcpy(context->video + 8U + context->parameter_sets.sps_length,
               context->parameter_sets.pps, context->parameter_sets.pps_length);
  (void)memcpy(context->video + 8U + context->parameter_sets.sps_length +
                   context->parameter_sets.pps_length,
               context->assembly, context->assembly_length);
  context->video_length = required;
  context->video_pts_ns = (uint64_t)timestamp * UINT64_C(1000000000) /
      UINT64_C(90000);
  context->video_keyframe = idr;
  context->video_ready = 1;
  context->assembly_length = 0U;
  return LS200_STATUS_OK;
}

static ls200_status native_handle_video(native_context *context,
                                        ls200_bytes bytes) {
  ls200_rtp_packet packet;
  ls200_mutable_bytes output;
  int complete = 0;
  int retried = 0;
  ls200_status status = ls200_rtp_parse(bytes, &packet);
  if (status != LS200_STATUS_OK ||
      packet.header.payload_type != context->h264_payload_type) {
    return LS200_STATUS_INVALID_DATA;
  }
retry:
  output.data = context->assembly + context->assembly_length;
  output.capacity = context->maximum_access_unit_bytes - context->assembly_length;
  output.length = 0U;
  status = ls200_h264_depacketizer_push(context->depacketizer, &packet, &output,
                                         &complete);
  if (status == LS200_STATUS_STATE_ERROR && retried == 0 &&
      native_h264_fu_start(&packet) != 0) {
    status = native_reset_video_assembly(context);
    if (status != LS200_STATUS_OK) return status;
    context->health.frames_dropped++;
    retried = 1;
    goto retry;
  }
  if (status == LS200_STATUS_INVALID_DATA &&
      native_h264_orphan_fragment(&packet) != 0) {
    context->assembly_length = 0U;
    context->video_synchronized = 0;
    context->health.frames_dropped++;
    return LS200_STATUS_AGAIN;
  }
  if (status != LS200_STATUS_OK) return status;
  context->assembly_length += output.length;
  return complete != 0 ? native_finish_video(context, packet.header.timestamp) :
      LS200_STATUS_AGAIN;
}

static ls200_status native_handle_audio(native_context *context,
                                        ls200_bytes bytes) {
  ls200_rtp_packet packet;
  ls200_mutable_bytes output = {context->pcm, context->maximum_pcm_frame_bytes, 0U};
  ls200_bytes access_unit;
  ls200_status status = ls200_rtp_parse(bytes, &packet);
  if (status != LS200_STATUS_OK ||
      packet.header.payload_type != context->aac_payload_type ||
      packet.payload.length < 4U) return LS200_STATUS_INVALID_DATA;
  status = ls200_rtsp_aac_au_from_payload(packet.payload, &access_unit);
  if (status != LS200_STATUS_OK) return status;
  if (context->discovery_only != 0) return LS200_STATUS_AGAIN;
  status = ls200_aac_decoder_decode(context->aac_decoder, access_unit, &output);
  if (status != LS200_STATUS_OK) return status;
  context->pcm_length = output.length;
  context->audio_pts_ns = (uint64_t)packet.header.timestamp * UINT64_C(1000000000) /
      (context->aac.sample_rate == 0U ? 8000U : context->aac.sample_rate);
  context->audio_ready = 1;
  return LS200_STATUS_OK;
}

void ls200_rtsp_native_clear_reorder(native_reorder_state *state) {
  if (state != NULL) (void)memset(state, 0, sizeof(*state));
}

static int native_sequence_after(uint16_t sequence, uint16_t expected) {
  return (int16_t)(sequence - expected) > 0;
}

static native_reorder_packet *native_reorder_find(native_reorder_state *state,
                                                   uint16_t sequence) {
  size_t index;
  for (index = 0U; index < LS200_RTSP_NATIVE_MAX_REORDER_PACKETS; ++index) {
    if (state->packets[index].occupied != 0 &&
        state->packets[index].sequence == sequence) return &state->packets[index];
  }
  return NULL;
}

static native_reorder_packet *native_reorder_slot(native_reorder_state *state) {
  size_t index;
  for (index = 0U; index < LS200_RTSP_NATIVE_MAX_REORDER_PACKETS; ++index) {
    if (state->packets[index].occupied == 0) return &state->packets[index];
  }
  return NULL;
}

static native_reorder_packet *native_reorder_next(native_reorder_state *state) {
  native_reorder_packet *next = NULL;
  uint16_t next_distance = 0U;
  size_t index;
  for (index = 0U; index < LS200_RTSP_NATIVE_MAX_REORDER_PACKETS; ++index) {
    uint16_t distance;
    if (state->packets[index].occupied == 0) continue;
    distance = (uint16_t)(state->packets[index].sequence -
                          state->expected_sequence);
    if (next == NULL || distance < next_distance) {
      next = &state->packets[index];
      next_distance = distance;
    }
  }
  return next;
}

static int native_reorder_has_packets(const native_reorder_state *state) {
  size_t index;
  for (index = 0U; index < LS200_RTSP_NATIVE_MAX_REORDER_PACKETS; ++index) {
    if (state->packets[index].occupied != 0) return 1;
  }
  return 0;
}

static void native_reorder_update_gap(native_reorder_state *state,
                                      uint64_t now_ns) {
  if (native_reorder_has_packets(state) == 0 ||
      native_reorder_find(state, state->expected_sequence) != NULL) {
    state->gap_started_ns = 0U;
  } else if (state->gap_started_ns == 0U) {
    state->gap_started_ns = now_ns;
  }
}

static ls200_status native_reorder_drain(native_context *context,
                                         native_reorder_state *state,
                                         native_rtp_handler handler) {
  native_reorder_packet *packet;
  ls200_status status = LS200_STATUS_AGAIN;
  for (;;) {
    packet = native_reorder_find(state, state->expected_sequence);
    if (packet == NULL) return status;
    status = handler(context, (ls200_bytes){packet->data, packet->length});
    packet->occupied = 0;
    state->expected_sequence = (uint16_t)(state->expected_sequence + 1U);
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
    if (status == LS200_STATUS_OK) return status;
  }
}

static ls200_status native_reorder_drop_gap(native_context *context,
                                            native_rtp_handler handler) {
  if (handler != native_handle_video) return LS200_STATUS_OK;
  return native_reset_video_assembly(context);
}

static int native_reorder_input_valid(const native_context *context,
                                      const native_reorder_state *state,
                                      native_rtp_handler handler, ls200_bytes bytes) {
  if (context == NULL || state == NULL || handler == NULL) return 0;
  if (bytes.data == NULL || bytes.length == 0U) return 0;
  return bytes.length <= LS200_SIPD_MAX_RTP_PACKET_BYTES;
}

static ls200_status native_reorder_expected(native_context *context,
                                            native_reorder_state *state,
                                            native_rtp_handler handler,
                                            ls200_bytes bytes,
                                            uint64_t now_ns) {
  ls200_status status = handler(context, bytes);
  ls200_status drained;
  state->expected_sequence = (uint16_t)(state->expected_sequence + 1U);
  state->gap_started_ns = 0U;
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  if (status == LS200_STATUS_OK) {
    native_reorder_update_gap(state, now_ns);
    return status;
  }
  drained = native_reorder_drain(context, state, handler);
  native_reorder_update_gap(state, now_ns);
  return drained == LS200_STATUS_AGAIN ? status : drained;
}

static ls200_status native_reorder_queue(native_context *context,
                                         native_reorder_state *state,
                                         native_rtp_handler handler,
                                         const ls200_rtp_packet *parsed,
                                         ls200_bytes bytes, uint64_t now_ns) {
  native_reorder_packet *slot;
  uint16_t distance;
  uint64_t maximum_gap_ns;
  ls200_status status;
  if (native_reorder_find(state, parsed->header.sequence_number) != NULL) {
    context->health.frames_dropped++;
    return LS200_STATUS_AGAIN;
  }
  distance = (uint16_t)(parsed->header.sequence_number -
                        state->expected_sequence);
  slot = native_reorder_slot(state);
  if (slot == NULL) { context->health.frames_dropped++; return LS200_STATUS_AGAIN; }
  (void)memcpy(slot->data, bytes.data, bytes.length);
  slot->length = bytes.length;
  slot->sequence = parsed->header.sequence_number;
  slot->occupied = 1;
  if (state->gap_started_ns == 0U) state->gap_started_ns = now_ns;
  maximum_gap_ns = (uint64_t)context->jitter_max_ms * UINT64_C(1000000);
  if (distance > LS200_RTSP_NATIVE_MAX_REORDER_PACKETS) {
    if (maximum_gap_ns != 0U) return LS200_STATUS_AGAIN;
    status = native_reorder_drop_gap(context, handler);
    if (status != LS200_STATUS_OK) return status;
    state->expected_sequence = parsed->header.sequence_number;
    state->gap_started_ns = 0U;
    status = native_reorder_drain(context, state, handler);
    native_reorder_update_gap(state, now_ns);
    return status;
  }
  if (now_ns < state->gap_started_ns) {
    state->gap_started_ns = now_ns;
    return LS200_STATUS_AGAIN;
  }
  if (maximum_gap_ns != 0U &&
      now_ns - state->gap_started_ns < maximum_gap_ns) {
    return LS200_STATUS_AGAIN;
  }
  context->health.frames_dropped++;
  {
    status = native_reorder_drop_gap(context, handler);
    if (status != LS200_STATUS_OK) return status;
  }
  state->expected_sequence = (uint16_t)(state->expected_sequence + 1U);
  state->gap_started_ns = now_ns;
  {
    status = native_reorder_drain(context, state, handler);
    native_reorder_update_gap(state, now_ns);
    return status;
  }
}

static ls200_status native_reorder_expire(native_context *context,
                                          native_reorder_state *state,
                                          native_rtp_handler handler,
                                          uint64_t now_ns) {
  uint64_t maximum_gap_ns;
  native_reorder_packet *next;
  ls200_status status;
  status = native_reorder_drain(context, state, handler);
  native_reorder_update_gap(state, now_ns);
  if (status != LS200_STATUS_AGAIN) return status;
  if (native_reorder_has_packets(state) == 0 || state->gap_started_ns == 0U) {
    return LS200_STATUS_AGAIN;
  }
  if (now_ns < state->gap_started_ns) {
    state->gap_started_ns = now_ns;
    return LS200_STATUS_AGAIN;
  }
  maximum_gap_ns = (uint64_t)context->jitter_max_ms * UINT64_C(1000000);
  if (maximum_gap_ns != 0U &&
      now_ns - state->gap_started_ns < maximum_gap_ns) {
    return LS200_STATUS_AGAIN;
  }
  context->health.frames_dropped++;
  status = native_reorder_drop_gap(context, handler);
  if (status != LS200_STATUS_OK) return status;
  next = native_reorder_next(state);
  if (next != NULL &&
      (uint16_t)(next->sequence - state->expected_sequence) >
          LS200_RTSP_NATIVE_MAX_REORDER_PACKETS) {
    state->expected_sequence = next->sequence;
  } else {
    state->expected_sequence = (uint16_t)(state->expected_sequence + 1U);
  }
  state->gap_started_ns = now_ns;
  status = native_reorder_drain(context, state, handler);
  native_reorder_update_gap(state, now_ns);
  return status;
}

ls200_status ls200_rtsp_native_expire_reorder(native_context *context) {
  uint64_t now_ns = 0U;
  ls200_status video_status;
  ls200_status audio_status;
  if (context == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK) {
    return LS200_STATUS_IO_ERROR;
  }
  video_status = context->video_ready != 0 ? LS200_STATUS_AGAIN :
      native_reorder_expire(context, &context->video_reorder,
                            native_handle_video, now_ns);
  if (video_status != LS200_STATUS_OK && video_status != LS200_STATUS_AGAIN) {
    return video_status;
  }
  audio_status = context->audio_ready != 0 ? LS200_STATUS_AGAIN :
      native_reorder_expire(context, &context->audio_reorder,
                            native_handle_audio, now_ns);
  if (audio_status != LS200_STATUS_OK && audio_status != LS200_STATUS_AGAIN) {
    return audio_status;
  }
  return video_status == LS200_STATUS_OK || audio_status == LS200_STATUS_OK ?
      LS200_STATUS_OK : LS200_STATUS_AGAIN;
}

static ls200_status native_reorder_push(native_context *context,
                                        native_reorder_state *state,
                                        native_rtp_handler handler,
                                        ls200_bytes bytes) {
  ls200_rtp_packet parsed;
  ls200_status status;
  uint64_t now_ns = 0U;
  if (native_reorder_input_valid(context, state, handler, bytes) == 0) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_rtp_parse(bytes, &parsed);
  if (status != LS200_STATUS_OK) return status;
  (void)ls200_platform_monotonic_now(&now_ns);
  if (state->expected_valid == 0) {
    state->expected_valid = 1;
    state->expected_sequence = parsed.header.sequence_number;
  }
  if (parsed.header.sequence_number == state->expected_sequence) return
      native_reorder_expected(context, state, handler, bytes, now_ns);
  if (native_sequence_after(parsed.header.sequence_number,
                            state->expected_sequence) == 0) {
    context->health.frames_dropped++;
    return LS200_STATUS_AGAIN;
  }
  return native_reorder_queue(context, state, handler, &parsed, bytes, now_ns);
}

ls200_status ls200_rtsp_native_push_video(native_context *context,
                                          ls200_bytes bytes) {
  return native_reorder_push(context, &context->video_reorder, native_handle_video,
                             bytes);
}

ls200_status ls200_rtsp_native_push_audio(native_context *context,
                                          ls200_bytes bytes) {
  return native_reorder_push(context, &context->audio_reorder, native_handle_audio,
                             bytes);
}

ls200_status ls200_rtsp_aac_au_from_payload(ls200_bytes payload,
                                             ls200_bytes *out_access_unit) {
  uint16_t au_headers_length;
  uint16_t au_header;
  size_t access_unit_length;
  if (payload.data == NULL || out_access_unit == NULL || payload.length < 4U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  au_headers_length = (uint16_t)(((uint16_t)payload.data[0] << 8U) |
                                 payload.data[1]);
  if (au_headers_length != 16U) return LS200_STATUS_UNSUPPORTED;
  au_header = (uint16_t)(((uint16_t)payload.data[2] << 8U) | payload.data[3]);
  if ((au_header & 7U) != 0U) return LS200_STATUS_UNSUPPORTED;
  access_unit_length = (size_t)(au_header >> 3U);
  if (access_unit_length == 0U || access_unit_length != payload.length - 4U ||
      access_unit_length > LS200_SIPD_MAX_AUDIO_FRAME_BYTES) {
    return LS200_STATUS_INVALID_DATA;
  }
  out_access_unit->data = payload.data + 4U;
  out_access_unit->length = access_unit_length;
  return LS200_STATUS_OK;
}
