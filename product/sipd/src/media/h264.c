#include "ls200_sipd/h264.h"

#include <stdlib.h>
#include <string.h>

static char ls200_h264_hex(uint8_t value) {
  value &= 0x0fU;
  return (char)(value < 10U ? ('0' + value) : ('a' + value - 10U));
}

static void ls200_h264_set_profile_level_id(ls200_h264_parameter_sets *sets,
                                             const uint8_t *sps, size_t length) {
  if (length < 4U) {
    sets->profile_level_id[0] = '\0';
    return;
  }
  sets->profile_level_id[0] = ls200_h264_hex((uint8_t)(sps[1] >> 4));
  sets->profile_level_id[1] = ls200_h264_hex(sps[1]);
  sets->profile_level_id[2] = ls200_h264_hex((uint8_t)(sps[2] >> 4));
  sets->profile_level_id[3] = ls200_h264_hex(sps[2]);
  sets->profile_level_id[4] = ls200_h264_hex((uint8_t)(sps[3] >> 4));
  sets->profile_level_id[5] = ls200_h264_hex(sps[3]);
  sets->profile_level_id[6] = '\0';
}

static int ls200_h264_start_code_at(ls200_bytes input, size_t offset,
                                    size_t *out_length) {
  if (offset + 3U <= input.length && input.data[offset] == 0U &&
      input.data[offset + 1U] == 0U && input.data[offset + 2U] == 1U) {
    *out_length = 3U;
    return 1;
  }
  if (offset + 4U <= input.length && input.data[offset] == 0U &&
      input.data[offset + 1U] == 0U && input.data[offset + 2U] == 0U &&
      input.data[offset + 3U] == 1U) {
    *out_length = 4U;
    return 1;
  }
  return 0;
}

static size_t ls200_h264_nal_end(ls200_bytes input, size_t start, size_t end) {
  while (end > start && input.data[end - 1U] == 0U) --end;
  return end;
}

static ls200_status ls200_h264_emit_packet(ls200_h264_packetizer *packetizer,
                                           const uint8_t *payload,
                                           size_t payload_length, int marker,
                                           ls200_h264_packet_callback callback,
                                           void *context) {
  ls200_rtp_packet packet;
  if (payload_length == 0U || payload_length > LS200_SIPD_MAX_RTP_PACKET_BYTES) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  packet.header.payload_type = packetizer->payload_type;
  packet.header.marker = (uint8_t)(marker != 0);
  packet.header.sequence_number = packetizer->sequence_number;
  packet.header.timestamp = packetizer->timestamp;
  packet.header.ssrc = packetizer->ssrc;
  packet.header.header_bytes = 12U;
  packet.payload.data = payload;
  packet.payload.length = payload_length;
  packetizer->sequence_number = (uint16_t)(packetizer->sequence_number + 1U);
  return callback(context, &packet);
}

ls200_status ls200_h264_annexb_iterator_init(ls200_h264_annexb_iterator *iterator,
                                             ls200_bytes input,
                                             uint32_t maximum_nal_bytes) {
  if (iterator == NULL || input.data == NULL || input.length == 0U ||
      maximum_nal_bytes == 0U ||
      maximum_nal_bytes > LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  iterator->input = input;
  iterator->offset = 0U;
  iterator->maximum_nal_bytes = maximum_nal_bytes;
  return LS200_STATUS_OK;
}

ls200_status ls200_h264_annexb_iterator_next(ls200_h264_annexb_iterator *iterator,
                                             ls200_h264_nal *out_nal) {
  size_t start_length;
  size_t nal_start;
  size_t cursor;
  size_t next_length;
  if (iterator == NULL || out_nal == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  while (iterator->offset < iterator->input.length &&
         !ls200_h264_start_code_at(iterator->input, iterator->offset, &start_length)) {
    if (iterator->input.data[iterator->offset] != 0U) {
      return LS200_STATUS_INVALID_DATA;
    }
    iterator->offset++;
  }
  if (iterator->offset == iterator->input.length) {
    return LS200_STATUS_END;
  }
  nal_start = iterator->offset + start_length;
  cursor = nal_start;
  while (cursor < iterator->input.length &&
         !ls200_h264_start_code_at(iterator->input, cursor, &next_length)) {
    cursor++;
  }
  iterator->offset = cursor;
  cursor = ls200_h264_nal_end(iterator->input, nal_start, cursor);
  if (cursor == nal_start) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (cursor - nal_start > (size_t)iterator->maximum_nal_bytes) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  out_nal->data = iterator->input.data + nal_start;
  out_nal->length = cursor - nal_start;
  out_nal->type = (uint8_t)(out_nal->data[0] & 0x1fU);
  out_nal->is_idr = out_nal->type == 5U;
  if ((out_nal->data[0] & 0x80U) != 0U) {
    return LS200_STATUS_INVALID_DATA;
  }
  if (out_nal->type == 0U || out_nal->type >= 24U) {
    return LS200_STATUS_INVALID_DATA;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_h264_parameter_sets_update(ls200_h264_parameter_sets *sets,
                                              const ls200_h264_nal *nal) {
  uint8_t *destination;
  size_t *destination_length;
  if (sets == NULL || nal == NULL || nal->data == NULL || nal->length == 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (nal->type == 7U) {
    destination = sets->sps;
    destination_length = &sets->sps_length;
  } else if (nal->type == 8U) {
    destination = sets->pps;
    destination_length = &sets->pps_length;
  } else {
    return LS200_STATUS_OK;
  }
  if (nal->length > 1024U) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  if (nal->type == 7U && nal->length < 4U) return LS200_STATUS_INVALID_DATA;
  if (*destination_length != nal->length ||
      memcmp(destination, nal->data, nal->length) != 0) {
    memcpy(destination, nal->data, nal->length);
    *destination_length = nal->length;
    if (nal->type == 7U) {
      ls200_h264_set_profile_level_id(sets, nal->data, nal->length);
    }
    sets->generation++;
  }
  sets->ready_for_idr = (sets->sps_length != 0U && sets->pps_length != 0U);
  return LS200_STATUS_OK;
}

ls200_status ls200_h264_profile_level_id(const ls200_h264_parameter_sets *sets,
                                         char out_profile_level_id[7]) {
  if (sets == NULL || out_profile_level_id == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (sets->sps_length == 0U || sets->profile_level_id[0] == '\0') return LS200_STATUS_AGAIN;
  (void)memcpy(out_profile_level_id, sets->profile_level_id, sizeof(sets->profile_level_id));
  return LS200_STATUS_OK;
}

int ls200_h264_can_transmit_access_unit(const ls200_h264_parameter_sets *sets,
                                        int access_unit_has_idr) {
  return sets != NULL && sets->ready_for_idr != 0 && access_unit_has_idr != 0;
}

ls200_status ls200_h264_packetize_single_nal(ls200_h264_packetizer *packetizer,
                                             const ls200_h264_nal *nal,
                                             ls200_h264_packet_callback callback,
                                             void *callback_context) {
  size_t maximum_payload;
  if (packetizer == NULL || nal == NULL || nal->data == NULL ||
      nal->length == 0U || callback == NULL || packetizer->mtu <= 12U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if ((nal->data[0] & 0x80U) != 0U) return LS200_STATUS_INVALID_DATA;
  maximum_payload = (size_t)packetizer->mtu - 12U;
  if (nal->length > maximum_payload) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  return ls200_h264_emit_packet(packetizer, nal->data, nal->length, 1, callback,
                                 callback_context);
}

ls200_status ls200_h264_packetize_fu_a(ls200_h264_packetizer *packetizer,
                                       const ls200_h264_nal *nal,
                                       ls200_h264_packet_callback callback,
                                       void *callback_context) {
  uint8_t fragment[LS200_SIPD_MAX_RTP_PACKET_BYTES];
  size_t maximum_fragment;
  size_t offset;
  ls200_status status;
  if (packetizer == NULL || nal == NULL || nal->data == NULL || nal->length < 2U ||
      callback == NULL || packetizer->mtu <= 14U ||
      packetizer->mtu > LS200_SIPD_MAX_RTP_PACKET_BYTES) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if ((nal->data[0] & 0x80U) != 0U) return LS200_STATUS_INVALID_DATA;
  maximum_fragment = (size_t)packetizer->mtu - 14U;
  /* RFC 6184 forbids setting both S and E on one FU. A NAL that fits is
   * transmitted directly, including when this entry point is called. */
  if (nal->length <= (size_t)packetizer->mtu - 12U)
    return ls200_h264_packetize_single_nal(packetizer, nal, callback, callback_context);
  offset = 1U;
  while (offset < nal->length) {
    size_t remaining = nal->length - offset;
    size_t fragment_length = remaining < maximum_fragment ? remaining : maximum_fragment;
    int start = offset == 1U;
    int end = fragment_length == remaining;
    fragment[0] = (uint8_t)((nal->data[0] & 0xe0U) | 28U);
    fragment[1] = (uint8_t)((nal->data[0] & 0x1fU) | (start ? 0x80U : 0U) |
                            (end ? 0x40U : 0U));
    memcpy(fragment + 2U, nal->data + offset, fragment_length);
    status = ls200_h264_emit_packet(packetizer, fragment, fragment_length + 2U, end,
                                    callback, callback_context);
    if (status != LS200_STATUS_OK) {
      return status;
    }
    offset += fragment_length;
  }
  return LS200_STATUS_OK;
}
