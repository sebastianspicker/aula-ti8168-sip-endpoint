#include "aula_sipd/dtmf.h"

aula_status aula_dtmf_validate_digit(uint8_t digit) {
  return digit <= 15U ? AULA_STATUS_OK : AULA_STATUS_INVALID_ARGUMENT;
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
