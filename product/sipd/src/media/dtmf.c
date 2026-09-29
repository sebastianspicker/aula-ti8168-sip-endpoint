#include "aula_sipd/dtmf.h"

#define AULA_DTMF_PACKET_SAMPLES 160U
#define AULA_DTMF_VOLUME 10U
#define AULA_DTMF_REPEAT_MASK 0x0fU
#define AULA_DTMF_DIGIT_SHIFT 4U

static uint8_t aula_dtmf_sender_digit(const aula_dtmf_sender *sender) {
  return (uint8_t)(sender->repeat_count >> AULA_DTMF_DIGIT_SHIFT);
}

static uint8_t aula_dtmf_sender_repeats(const aula_dtmf_sender *sender) {
  return (uint8_t)(sender->repeat_count & AULA_DTMF_REPEAT_MASK);
}

static void aula_dtmf_set_sender_state(aula_dtmf_sender *sender, uint8_t digit,
                                         uint8_t repeats) {
  sender->repeat_count = (uint8_t)(((uint32_t)digit << AULA_DTMF_DIGIT_SHIFT) |
                                   ((uint32_t)repeats & AULA_DTMF_REPEAT_MASK));
}

static aula_status aula_dtmf_write_packet(aula_dtmf_sender *sender,
                                             uint8_t digit, int end,
                                             aula_mutable_bytes *output) {
  if (output->capacity < 16U) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  output->data[0] = 0x80U;
  output->data[1] = sender->payload_type;
  output->data[2] = (uint8_t)(sender->sequence_number >> 8U);
  output->data[3] = (uint8_t)sender->sequence_number;
  output->data[4] = (uint8_t)(sender->rtp_timestamp >> 24U);
  output->data[5] = (uint8_t)(sender->rtp_timestamp >> 16U);
  output->data[6] = (uint8_t)(sender->rtp_timestamp >> 8U);
  output->data[7] = (uint8_t)sender->rtp_timestamp;
  output->data[8] = (uint8_t)(sender->ssrc >> 24U);
  output->data[9] = (uint8_t)(sender->ssrc >> 16U);
  output->data[10] = (uint8_t)(sender->ssrc >> 8U);
  output->data[11] = (uint8_t)sender->ssrc;
  output->data[12] = digit;
  output->data[13] = (uint8_t)(AULA_DTMF_VOLUME | (end != 0 ? 0x80U : 0U));
  output->data[14] = (uint8_t)(sender->duration_samples >> 8U);
  output->data[15] = (uint8_t)sender->duration_samples;
  output->length = 16U;
  sender->sequence_number = (uint16_t)(sender->sequence_number + 1U);
  return AULA_STATUS_OK;
}

aula_status aula_dtmf_validate_digit(uint8_t digit) {
  return digit <= 15U ? AULA_STATUS_OK : AULA_STATUS_INVALID_ARGUMENT;
}

aula_status aula_dtmf_sender_start(aula_dtmf_sender *sender, uint8_t digit,
                                     aula_mutable_bytes *output) {
  aula_status status;
  if (sender == NULL || output == NULL || output->data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  status = aula_dtmf_validate_digit(digit);
  if (status != AULA_STATUS_OK) {
    return status;
  }
  if (sender->maximum_duration_samples == 0U) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  sender->duration_samples = 0U;
  aula_dtmf_set_sender_state(sender, digit, 0U);
  return aula_dtmf_write_packet(sender, digit, 0, output);
}

aula_status aula_dtmf_sender_advance(aula_dtmf_sender *sender, int end,
                                       aula_mutable_bytes *output) {
  uint8_t digit;
  uint8_t repeats;
  if (sender == NULL || output == NULL || output->data == NULL ||
      sender->maximum_duration_samples == 0U) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  digit = aula_dtmf_sender_digit(sender);
  repeats = aula_dtmf_sender_repeats(sender);
  if (aula_dtmf_validate_digit(digit) != AULA_STATUS_OK) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (end != 0) {
    if (repeats >= 3U) {
      return AULA_STATUS_END;
    }
    if (sender->duration_samples == 0U) {
      if (sender->maximum_duration_samples < AULA_DTMF_PACKET_SAMPLES) {
        return AULA_STATUS_LIMIT_EXCEEDED;
      }
      sender->duration_samples = AULA_DTMF_PACKET_SAMPLES;
    }
    repeats++;
    aula_dtmf_set_sender_state(sender, digit, repeats);
    return aula_dtmf_write_packet(sender, digit, 1, output);
  }
  if (sender->duration_samples > sender->maximum_duration_samples -
      AULA_DTMF_PACKET_SAMPLES) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  sender->duration_samples = (uint16_t)(sender->duration_samples +
                                         AULA_DTMF_PACKET_SAMPLES);
  return aula_dtmf_write_packet(sender, digit, 0, output);
}

aula_status aula_dtmf_account_received(const aula_rtp_packet *packet,
                                         aula_dtmf_receive_stats *stats,
                                         aula_dtmf_event *out_event) {
  uint8_t digit;
  uint8_t flags;
  uint16_t duration;
  if (packet == NULL || stats == NULL || out_event == NULL ||
      packet->payload.data == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (packet->payload.length != 4U) {
    stats->malformed_events++;
    return AULA_STATUS_INVALID_DATA;
  }
  digit = packet->payload.data[0];
  flags = packet->payload.data[1];
  duration = (uint16_t)(((uint16_t)packet->payload.data[2] << 8U) |
                        packet->payload.data[3]);
  if (aula_dtmf_validate_digit(digit) != AULA_STATUS_OK || (flags & 0x40U) != 0U) {
    stats->rejected_events++;
    return AULA_STATUS_INVALID_DATA;
  }
  out_event->digit = digit;
  out_event->duration_samples = duration;
  out_event->end = (flags & 0x80U) != 0U;
  stats->accepted_events++;
  return AULA_STATUS_OK;
}
