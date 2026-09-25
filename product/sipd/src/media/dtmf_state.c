#include "ls200_sipd/dtmf.h"

#include <stdlib.h>
#include <string.h>

#define LS200_DTMF_MAX_HISTORY 128U
#define LS200_DTMF_VOLUME 10U

struct ls200_dtmf_sender_state {
  ls200_dtmf_sender_config config;
  ls200_dtmf_event active;
  uint16_t sequence_number;
  uint32_t timestamp;
  uint8_t end_packets;
  int active_set;
};

typedef struct ls200_dtmf_history_entry {
  uint32_t ssrc;
  uint32_t timestamp;
  uint8_t digit;
  uint16_t duration;
  int end;
} ls200_dtmf_history_entry;

struct ls200_dtmf_receiver {
  ls200_dtmf_receiver_config config;
  ls200_dtmf_receive_stats stats;
  ls200_dtmf_history_entry *history;
  uint32_t history_count;
  uint32_t history_next;
};

static ls200_status ls200_dtmf_state_write(ls200_dtmf_sender_state *sender,
                                           int end, ls200_mutable_bytes *output) {
  if (output == NULL || output->data == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (output->capacity < 16U) return LS200_STATUS_LIMIT_EXCEEDED;
  output->data[0] = 0x80U;
  output->data[1] = sender->config.payload_type;
  output->data[2] = (uint8_t)(sender->sequence_number >> 8);
  output->data[3] = (uint8_t)sender->sequence_number;
  output->data[4] = (uint8_t)(sender->timestamp >> 24);
  output->data[5] = (uint8_t)(sender->timestamp >> 16);
  output->data[6] = (uint8_t)(sender->timestamp >> 8);
  output->data[7] = (uint8_t)sender->timestamp;
  output->data[8] = (uint8_t)(sender->config.ssrc >> 24);
  output->data[9] = (uint8_t)(sender->config.ssrc >> 16);
  output->data[10] = (uint8_t)(sender->config.ssrc >> 8);
  output->data[11] = (uint8_t)sender->config.ssrc;
  output->data[12] = sender->active.digit;
  output->data[13] = (uint8_t)(LS200_DTMF_VOLUME | (end != 0 ? 0x80U : 0U));
  output->data[14] = (uint8_t)(sender->active.duration_samples >> 8);
  output->data[15] = (uint8_t)sender->active.duration_samples;
  output->length = 16U;
  sender->sequence_number = (uint16_t)(sender->sequence_number + 1U);
  return LS200_STATUS_OK;
}

ls200_status ls200_dtmf_sender_create(const ls200_dtmf_sender_config *config,
                                      ls200_dtmf_sender_state **out_sender) {
  ls200_dtmf_sender_state *sender;
  if (config == NULL || out_sender == NULL || *out_sender != NULL || config->payload_type > 127U ||
      config->maximum_duration_samples == 0U || config->end_packet_repetitions == 0U ||
      config->end_packet_repetitions > 15U) return LS200_STATUS_INVALID_ARGUMENT;
  sender = (ls200_dtmf_sender_state *)calloc(1U, sizeof(*sender));
  if (sender == NULL) return LS200_STATUS_INTERNAL_ERROR;
  sender->config = *config;
  sender->sequence_number = config->initial_sequence_number;
  *out_sender = sender;
  return LS200_STATUS_OK;
}

ls200_status ls200_dtmf_sender_begin(ls200_dtmf_sender_state *sender,
                                     uint8_t digit, uint32_t rtp_timestamp) {
  ls200_status status;
  if (sender == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (sender->active_set) return LS200_STATUS_STATE_ERROR;
  status = ls200_dtmf_validate_digit(digit);
  if (status != LS200_STATUS_OK) return status;
  sender->active.digit = digit;
  sender->active.duration_samples = 0U;
  sender->active.end = 0;
  sender->timestamp = rtp_timestamp;
  sender->end_packets = 0U;
  sender->active_set = 1;
  return LS200_STATUS_OK;
}

ls200_status ls200_dtmf_sender_emit(ls200_dtmf_sender_state *sender,
                                    uint16_t duration_samples, int end,
                                    ls200_mutable_bytes *output) {
  ls200_status status;
  if (sender == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (!sender->active_set) return LS200_STATUS_END;
  if (duration_samples < sender->active.duration_samples || duration_samples == 0U ||
      duration_samples > sender->config.maximum_duration_samples) return LS200_STATUS_LIMIT_EXCEEDED;
  if (end != 0) {
    if (sender->end_packets >= sender->config.end_packet_repetitions) return LS200_STATUS_END;
    sender->end_packets++;
    sender->active.end = 1;
  } else if (sender->end_packets != 0U) {
    return LS200_STATUS_STATE_ERROR;
  }
  sender->active.duration_samples = duration_samples;
  status = ls200_dtmf_state_write(sender, end != 0, output);
  if (status == LS200_STATUS_OK && end != 0 &&
      sender->end_packets == sender->config.end_packet_repetitions) sender->active_set = 0;
  return status;
}

ls200_status ls200_dtmf_sender_get_active(const ls200_dtmf_sender_state *sender,
                                          ls200_dtmf_event *out_event) {
  if (sender == NULL || out_event == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (!sender->active_set) return LS200_STATUS_END;
  *out_event = sender->active;
  return LS200_STATUS_OK;
}

void ls200_dtmf_sender_destroy(ls200_dtmf_sender_state *sender) {
  if (sender != NULL) { (void)memset(sender, 0, sizeof(*sender)); free(sender); }
}

ls200_status ls200_dtmf_receiver_create(const ls200_dtmf_receiver_config *config,
                                        ls200_dtmf_receiver **out_receiver) {
  ls200_dtmf_receiver *receiver;
  if (config == NULL || out_receiver == NULL || *out_receiver != NULL ||
      config->duplicate_history_entries == 0U || config->duplicate_history_entries > LS200_DTMF_MAX_HISTORY ||
      config->maximum_duration_samples == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  receiver = (ls200_dtmf_receiver *)calloc(1U, sizeof(*receiver));
  if (receiver == NULL) return LS200_STATUS_INTERNAL_ERROR;
  receiver->history = (ls200_dtmf_history_entry *)calloc(config->duplicate_history_entries,
                                                           sizeof(*receiver->history));
  if (receiver->history == NULL) { free(receiver); return LS200_STATUS_INTERNAL_ERROR; }
  receiver->config = *config;
  *out_receiver = receiver;
  return LS200_STATUS_OK;
}

ls200_status ls200_dtmf_receiver_accept(ls200_dtmf_receiver *receiver,
                                        const ls200_rtp_packet *packet,
                                        ls200_dtmf_event *out_event) {
  ls200_dtmf_event event;
  ls200_status status;
  uint32_t index;
  if (receiver == NULL || packet == NULL || out_event == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = ls200_dtmf_account_received(packet, &receiver->stats, &event);
  if (status != LS200_STATUS_OK) return status;
  if (event.duration_samples == 0U || event.duration_samples > receiver->config.maximum_duration_samples) {
    receiver->stats.accepted_events--;
    receiver->stats.rejected_events++;
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  for (index = 0U; index < receiver->history_count; ++index) {
    ls200_dtmf_history_entry *prior = &receiver->history[index];
    if (prior->ssrc == packet->header.ssrc && prior->timestamp == packet->header.timestamp &&
        prior->digit == event.digit && prior->duration == event.duration_samples &&
        prior->end == event.end) {
      receiver->stats.accepted_events--;
      receiver->stats.duplicate_events++;
      *out_event = event;
      return LS200_STATUS_AGAIN;
    }
  }
  receiver->history[receiver->history_next].ssrc = packet->header.ssrc;
  receiver->history[receiver->history_next].timestamp = packet->header.timestamp;
  receiver->history[receiver->history_next].digit = event.digit;
  receiver->history[receiver->history_next].duration = event.duration_samples;
  receiver->history[receiver->history_next].end = event.end;
  receiver->history_next = (receiver->history_next + 1U) % receiver->config.duplicate_history_entries;
  if (receiver->history_count < receiver->config.duplicate_history_entries) receiver->history_count++;
  *out_event = event;
  return LS200_STATUS_OK;
}

ls200_status ls200_dtmf_receiver_get_stats(const ls200_dtmf_receiver *receiver,
                                           ls200_dtmf_receive_stats *out_stats) {
  if (receiver == NULL || out_stats == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_stats = receiver->stats;
  return LS200_STATUS_OK;
}

void ls200_dtmf_receiver_destroy(ls200_dtmf_receiver *receiver) {
  if (receiver != NULL) { free(receiver->history); (void)memset(receiver, 0, sizeof(*receiver)); free(receiver); }
}
