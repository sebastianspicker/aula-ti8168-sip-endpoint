#ifndef LS200_SIPD_DTMF_H
#define LS200_SIPD_DTMF_H

#include "ls200_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_dtmf_sender {
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t sequence_number;
  uint32_t rtp_timestamp;
  uint16_t duration_samples;
  uint16_t maximum_duration_samples;
  uint8_t repeat_count;
} ls200_dtmf_sender;

typedef struct ls200_dtmf_event {
  uint8_t digit;
  uint16_t duration_samples;
  int end;
} ls200_dtmf_event;

typedef struct ls200_dtmf_receive_stats {
  uint64_t accepted_events;
  uint64_t duplicate_events;
  uint64_t malformed_events;
  uint64_t rejected_events;
} ls200_dtmf_receive_stats;

typedef struct ls200_dtmf_sender_config {
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t initial_sequence_number;
  uint16_t maximum_duration_samples;
  uint8_t end_packet_repetitions;
} ls200_dtmf_sender_config;

typedef struct ls200_dtmf_receiver_config {
  uint32_t duplicate_history_entries;
  uint16_t maximum_duration_samples;
} ls200_dtmf_receiver_config;

typedef struct ls200_dtmf_sender_state ls200_dtmf_sender_state;
typedef struct ls200_dtmf_receiver ls200_dtmf_receiver;

ls200_status ls200_dtmf_validate_digit(uint8_t digit);
ls200_status ls200_dtmf_sender_start(ls200_dtmf_sender *sender, uint8_t digit,
                                     ls200_mutable_bytes *output);
ls200_status ls200_dtmf_sender_advance(ls200_dtmf_sender *sender, int end,
                                       ls200_mutable_bytes *output);
ls200_status ls200_dtmf_account_received(const ls200_rtp_packet *packet,
                                         ls200_dtmf_receive_stats *stats,
                                         ls200_dtmf_event *out_event);
/* Stateful APIs retain one active event and a finite duplicate-event history. */
ls200_status ls200_dtmf_sender_create(const ls200_dtmf_sender_config *config,
                                      ls200_dtmf_sender_state **out_sender);
ls200_status ls200_dtmf_sender_begin(ls200_dtmf_sender_state *sender,
                                     uint8_t digit, uint32_t rtp_timestamp);
ls200_status ls200_dtmf_sender_emit(ls200_dtmf_sender_state *sender,
                                    uint16_t duration_samples, int end,
                                    ls200_mutable_bytes *output);
ls200_status ls200_dtmf_sender_get_active(const ls200_dtmf_sender_state *sender,
                                          ls200_dtmf_event *out_event);
void ls200_dtmf_sender_destroy(ls200_dtmf_sender_state *sender);
ls200_status ls200_dtmf_receiver_create(const ls200_dtmf_receiver_config *config,
                                        ls200_dtmf_receiver **out_receiver);
ls200_status ls200_dtmf_receiver_accept(ls200_dtmf_receiver *receiver,
                                        const ls200_rtp_packet *packet,
                                        ls200_dtmf_event *out_event);
ls200_status ls200_dtmf_receiver_get_stats(const ls200_dtmf_receiver *receiver,
                                           ls200_dtmf_receive_stats *out_stats);
void ls200_dtmf_receiver_destroy(ls200_dtmf_receiver *receiver);

#ifdef __cplusplus
}
#endif

#endif
