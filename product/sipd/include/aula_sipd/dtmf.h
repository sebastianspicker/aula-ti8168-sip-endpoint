#ifndef AULA_SIPD_DTMF_H
#define AULA_SIPD_DTMF_H

#include "aula_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_dtmf_sender {
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t sequence_number;
  uint32_t rtp_timestamp;
  uint16_t duration_samples;
  uint16_t maximum_duration_samples;
  uint8_t repeat_count;
} aula_dtmf_sender;

typedef struct aula_dtmf_event {
  uint8_t digit;
  uint16_t duration_samples;
  int end;
} aula_dtmf_event;

typedef struct aula_dtmf_receive_stats {
  uint64_t accepted_events;
  uint64_t duplicate_events;
  uint64_t malformed_events;
  uint64_t rejected_events;
} aula_dtmf_receive_stats;

typedef struct aula_dtmf_sender_config {
  uint8_t payload_type;
  uint32_t ssrc;
  uint16_t initial_sequence_number;
  uint16_t maximum_duration_samples;
  uint8_t end_packet_repetitions;
} aula_dtmf_sender_config;

typedef struct aula_dtmf_receiver_config {
  uint32_t duplicate_history_entries;
  uint16_t maximum_duration_samples;
} aula_dtmf_receiver_config;

typedef struct aula_dtmf_sender_state aula_dtmf_sender_state;
typedef struct aula_dtmf_receiver aula_dtmf_receiver;

aula_status aula_dtmf_validate_digit(uint8_t digit);
aula_status aula_dtmf_sender_start(aula_dtmf_sender *sender, uint8_t digit,
                                     aula_mutable_bytes *output);
aula_status aula_dtmf_sender_advance(aula_dtmf_sender *sender, int end,
                                       aula_mutable_bytes *output);
aula_status aula_dtmf_account_received(const aula_rtp_packet *packet,
                                         aula_dtmf_receive_stats *stats,
                                         aula_dtmf_event *out_event);
/* Stateful APIs retain one active event and a finite duplicate-event history. */
aula_status aula_dtmf_sender_create(const aula_dtmf_sender_config *config,
                                      aula_dtmf_sender_state **out_sender);
aula_status aula_dtmf_sender_begin(aula_dtmf_sender_state *sender,
                                     uint8_t digit, uint32_t rtp_timestamp);
aula_status aula_dtmf_sender_emit(aula_dtmf_sender_state *sender,
                                    uint16_t duration_samples, int end,
                                    aula_mutable_bytes *output);
aula_status aula_dtmf_sender_get_active(const aula_dtmf_sender_state *sender,
                                          aula_dtmf_event *out_event);
void aula_dtmf_sender_destroy(aula_dtmf_sender_state *sender);
aula_status aula_dtmf_receiver_create(const aula_dtmf_receiver_config *config,
                                        aula_dtmf_receiver **out_receiver);
aula_status aula_dtmf_receiver_accept(aula_dtmf_receiver *receiver,
                                        const aula_rtp_packet *packet,
                                        aula_dtmf_event *out_event);
aula_status aula_dtmf_receiver_get_stats(const aula_dtmf_receiver *receiver,
                                           aula_dtmf_receive_stats *out_stats);
void aula_dtmf_receiver_destroy(aula_dtmf_receiver *receiver);

#ifdef __cplusplus
}
#endif

#endif
