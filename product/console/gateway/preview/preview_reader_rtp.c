#include "preview_reader_internal.h"

#include <string.h>

#define PREVIEW_MAX_TIMESTAMP_STEP_90KHZ 900000U
#define PREVIEW_RTP_FIXED_HEADER_BYTES 12U

typedef struct preview_rtp_packet {
  uint8_t payload_type;
  uint8_t marker;
  uint16_t sequence;
  uint32_t timestamp;
  uint32_t ssrc;
  aula_bytes payload;
} preview_rtp_packet;

static uint16_t read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8U) | (uint16_t)data[1]);
}

static uint32_t read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
         ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static aula_status parse_rtp(aula_bytes input, preview_rtp_packet *out_packet) {
  const uint8_t *data;
  size_t header_length;
  size_t extension_length;
  size_t padding_length = 0U;
  uint8_t csrc_count;
  if (out_packet == NULL || input.data == NULL ||
      input.length < PREVIEW_RTP_FIXED_HEADER_BYTES) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (input.length > AULA_SIPD_MAX_RTP_PACKET_BYTES) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  data = input.data;
  if ((data[0] >> 6U) != 2U) return AULA_STATUS_INVALID_DATA;
  csrc_count = (uint8_t)(data[0] & 0x0fU);
  header_length = PREVIEW_RTP_FIXED_HEADER_BYTES + ((size_t)csrc_count * 4U);
  if (header_length > input.length) return AULA_STATUS_INVALID_DATA;
  if ((data[0] & 0x10U) != 0U) {
    if (input.length - header_length < 4U) return AULA_STATUS_INVALID_DATA;
    extension_length = 4U + ((size_t)read_u16(data + header_length + 2U) * 4U);
    if (extension_length > input.length - header_length) {
      return AULA_STATUS_INVALID_DATA;
    }
    header_length += extension_length;
  }
  if ((data[0] & 0x20U) != 0U) {
    padding_length = data[input.length - 1U];
    if (padding_length == 0U || padding_length > input.length - header_length) {
      return AULA_STATUS_INVALID_DATA;
    }
  }
  if (input.length == header_length + padding_length) {
    return AULA_STATUS_INVALID_DATA;
  }
  out_packet->payload_type = (uint8_t)(data[1] & 0x7fU);
  out_packet->marker = (uint8_t)(data[1] >> 7U);
  out_packet->sequence = read_u16(data + 2U);
  out_packet->timestamp = read_u32(data + 4U);
  out_packet->ssrc = read_u32(data + 8U);
  out_packet->payload.data = data + header_length;
  out_packet->payload.length = input.length - header_length - padding_length;
  return AULA_STATUS_OK;
}

static aula_status recreate_depacketizer(aula_preview_reader *reader) {
  aula_h264_depacketizer *replacement = NULL;
  aula_status status = aula_h264_depacketizer_create(
      reader->config.maximum_access_unit_bytes, &replacement);
  if (status != AULA_STATUS_OK) return status;
  aula_h264_depacketizer_destroy(reader->depacketizer);
  reader->depacketizer = replacement;
  reader->assembly_length = 0U;
  reader->au_active = 0;
  return AULA_STATUS_OK;
}

static aula_status note_discontinuity(aula_preview_reader *reader,
                                       int clear_parameter_sets) {
  aula_status status = recreate_depacketizer(reader);
  if (status != AULA_STATUS_OK) return status;
  reader->decoder_ready = 0;
  if (reader->ever_emitted != 0) reader->discontinuity_pending = 1;
  if (clear_parameter_sets != 0) {
    aula_preview_secure_zero(&reader->parameter_sets,
                              sizeof(reader->parameter_sets));
  }
  return AULA_STATUS_OK;
}

static aula_status lock_source_and_sequence(aula_preview_reader *reader,
                                             const preview_rtp_packet *packet,
                                             int *out_gap) {
  aula_status status;
  *out_gap = 0;
  if (reader->ssrc_locked == 0) {
    reader->locked_ssrc = packet->ssrc;
    reader->ssrc_locked = 1;
  } else if (reader->locked_ssrc != packet->ssrc) {
    status = note_discontinuity(reader, 1);
    if (status != AULA_STATUS_OK) return status;
    reader->locked_ssrc = packet->ssrc;
    reader->sequence_locked = 0;
    reader->timestamp_locked = 0;
    *out_gap = 1;
  }
  if (reader->sequence_locked != 0 && packet->sequence != reader->next_sequence) {
    status = note_discontinuity(reader, 0);
    if (status != AULA_STATUS_OK) return status;
    *out_gap = 1;
  }
  reader->next_sequence = (uint16_t)(packet->sequence + 1U);
  reader->sequence_locked = 1;
  return AULA_STATUS_OK;
}

static aula_status begin_timestamp(aula_preview_reader *reader,
                                    const preview_rtp_packet *packet,
                                    int *out_gap) {
  aula_status status;
  int32_t delta;
  if (reader->au_active != 0 && packet->timestamp == reader->current_timestamp) {
    return AULA_STATUS_OK;
  }
  if (reader->au_active != 0) {
    status = note_discontinuity(reader, 0);
    if (status != AULA_STATUS_OK) return status;
    *out_gap = 1;
  }
  if (reader->timestamp_locked == 0) {
    reader->current_timestamp_90khz = reader->ever_emitted != 0
        ? reader->last_timestamp_90khz + 1U : 0U;
  } else {
    delta = (int32_t)(packet->timestamp - reader->last_timestamp);
    if (delta < 0 || (uint32_t)delta > PREVIEW_MAX_TIMESTAMP_STEP_90KHZ) {
      status = note_discontinuity(reader, 0);
      if (status != AULA_STATUS_OK) return status;
      reader->current_timestamp_90khz = reader->last_timestamp_90khz + 1U;
      *out_gap = 1;
    } else {
      reader->current_timestamp_90khz = reader->last_timestamp_90khz +
                                         (uint32_t)delta;
    }
  }
  reader->current_timestamp = packet->timestamp;
  reader->au_active = 1;
  return AULA_STATUS_OK;
}

static aula_status begin_packet(aula_preview_reader *reader,
                                 const preview_rtp_packet *packet,
                                 int *out_recoverable_gap) {
  aula_status status = lock_source_and_sequence(reader, packet,
                                                 out_recoverable_gap);
  if (status != AULA_STATUS_OK) return status;
  return begin_timestamp(reader, packet, out_recoverable_gap);
}

static aula_status inspect_access_unit(aula_preview_reader *reader,
                                        int *out_has_idr,
                                        int *out_has_vcl,
                                        int *out_parameter_changed) {
  aula_h264_annexb_iterator iterator;
  aula_h264_nal nal;
  aula_status status;
  uint32_t old_generation;
  *out_has_idr = 0;
  *out_has_vcl = 0;
  *out_parameter_changed = 0;
  status = aula_h264_annexb_iterator_init(
      &iterator, (aula_bytes){reader->assembly, reader->assembly_length},
      reader->config.maximum_access_unit_bytes);
  if (status != AULA_STATUS_OK) return status;
  for (;;) {
    status = aula_h264_annexb_iterator_next(&iterator, &nal);
    if (status == AULA_STATUS_END) return AULA_STATUS_OK;
    if (status != AULA_STATUS_OK) return status;
    old_generation = reader->parameter_sets.generation;
    status = aula_h264_parameter_sets_update(&reader->parameter_sets, &nal);
    if (status != AULA_STATUS_OK) return status;
    if (reader->parameter_sets.generation != old_generation) {
      if (nal.type == 7U) {
        reader->parameter_sets.pps_length = 0U;
        reader->parameter_sets.ready_for_idr = 0;
      }
      reader->parameter_set_generation++;
      *out_parameter_changed = 1;
    }
    if (nal.type >= 1U && nal.type <= 5U) *out_has_vcl = 1;
    if (nal.is_idr != 0) *out_has_idr = 1;
  }
}

static void populate_access_unit(aula_preview_reader *reader,
                                 aula_preview_access_unit *out_unit,
                                 int has_idr) {
  out_unit->annex_b.data = reader->assembly;
  out_unit->annex_b.length = reader->assembly_length;
  out_unit->sps.data = reader->parameter_sets.sps;
  out_unit->sps.length = reader->parameter_sets.sps_length;
  out_unit->pps.data = reader->parameter_sets.pps;
  out_unit->pps.length = reader->parameter_sets.pps_length;
  out_unit->timestamp_90khz = reader->current_timestamp_90khz;
  out_unit->parameter_set_generation = reader->parameter_set_generation;
  out_unit->keyframe = has_idr;
  out_unit->discontinuity = reader->ever_emitted != 0 &&
                            reader->discontinuity_pending != 0;
  reader->ever_emitted = 1;
  reader->discontinuity_pending = 0;
}

static aula_status finish_access_unit(aula_preview_reader *reader,
                                       aula_preview_access_unit *out_unit) {
  aula_status status;
  int has_idr;
  int has_vcl;
  int parameter_changed;
  reader->au_active = 0;
  reader->last_timestamp = reader->current_timestamp;
  reader->last_timestamp_90khz = reader->current_timestamp_90khz;
  reader->timestamp_locked = 1;
  if (reader->assembly_length == 0U) return AULA_STATUS_INVALID_DATA;
  status = inspect_access_unit(reader, &has_idr, &has_vcl,
                               &parameter_changed);
  if (status != AULA_STATUS_OK) return status;
  if (parameter_changed != 0) {
    reader->decoder_ready = 0;
    if (reader->ever_emitted != 0) reader->discontinuity_pending = 1;
  }
  if (reader->parameter_sets.ready_for_idr == 0 || has_vcl == 0 ||
      (reader->decoder_ready == 0 && has_idr == 0)) {
    reader->assembly_length = 0U;
    return AULA_STATUS_AGAIN;
  }
  if (has_idr != 0) reader->decoder_ready = 1;
  populate_access_unit(reader, out_unit, has_idr);
  reader->assembly_length = 0U;
  return AULA_STATUS_OK;
}

static aula_status depacketize(aula_preview_reader *reader,
                                const preview_rtp_packet *parsed,
                                int recoverable_gap,
                                aula_preview_access_unit *out_unit) {
  aula_rtp_packet packet;
  aula_mutable_bytes output;
  aula_status status;
  int complete = 0;
  (void)memset(&packet, 0, sizeof(packet));
  packet.header.payload_type = parsed->payload_type;
  packet.header.marker = parsed->marker;
  packet.header.sequence_number = parsed->sequence;
  packet.header.timestamp = parsed->timestamp;
  packet.header.ssrc = parsed->ssrc;
  packet.header.header_bytes = PREVIEW_RTP_FIXED_HEADER_BYTES;
  packet.payload = parsed->payload;
  output.data = reader->assembly + reader->assembly_length;
  output.capacity = reader->config.maximum_access_unit_bytes -
                    reader->assembly_length;
  output.length = 0U;
  status = aula_h264_depacketizer_push(reader->depacketizer, &packet, &output,
                                        &complete);
  if (recoverable_gap != 0 && status != AULA_STATUS_OK) {
    reader->drop_until_marker = parsed->marker == 0U;
    reader->au_active = 0;
    return AULA_STATUS_AGAIN;
  }
  if (status != AULA_STATUS_OK) return status;
  reader->assembly_length += output.length;
  return complete == 0 ? AULA_STATUS_AGAIN :
                         finish_access_unit(reader, out_unit);
}

static aula_status discard_broken_access_unit(
    aula_preview_reader *reader, const preview_rtp_packet *packet) {
  aula_status status;
  if (reader->ssrc_locked != 0 && packet->ssrc != reader->locked_ssrc) {
    status = note_discontinuity(reader, 1);
    if (status != AULA_STATUS_OK) return status;
    reader->locked_ssrc = packet->ssrc;
  }
  reader->ssrc_locked = 1;
  reader->next_sequence = (uint16_t)(packet->sequence + 1U);
  reader->sequence_locked = 1;
  if (packet->marker != 0U) {
    reader->drop_until_marker = 0;
    return recreate_depacketizer(reader) == AULA_STATUS_OK ?
        AULA_STATUS_AGAIN : AULA_STATUS_INTERNAL_ERROR;
  }
  return AULA_STATUS_AGAIN;
}

aula_status aula_preview_consume_rtp(
    aula_preview_reader *reader, aula_bytes bytes,
    aula_preview_access_unit *out_unit) {
  preview_rtp_packet parsed;
  aula_status status = parse_rtp(bytes, &parsed);
  int recoverable_gap = 0;
  if (status != AULA_STATUS_OK) return status;
  if (parsed.payload_type != reader->payload_type) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (reader->drop_until_marker != 0) {
    return discard_broken_access_unit(reader, &parsed);
  }
  status = begin_packet(reader, &parsed, &recoverable_gap);
  if (status != AULA_STATUS_OK) return status;
  return depacketize(reader, &parsed, recoverable_gap, out_unit);
}
