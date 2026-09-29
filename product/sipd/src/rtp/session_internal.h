#ifndef AULA_SIPD_RTP_SESSION_INTERNAL_H
#define AULA_SIPD_RTP_SESSION_INTERNAL_H

#include "aula_sipd/rtp.h"
#include "srtp_private.h"

#include <sys/socket.h>

struct aula_rtp_retransmit_cache;

struct aula_rtp_session {
  struct aula_rtp_retransmit_cache *retransmit;
  aula_rtp_port_pair ports;
  aula_rtp_identity identity;
  aula_rtp_source source;
  aula_rtp_source rtcp_source;
  aula_rtp_remote_target remote_target;
  aula_rtp_counters counters;
  uint8_t local_address[16];
  uint8_t local_address_length;
  uint8_t accepted_payloads[128];
  size_t accepted_payload_count;
  uint8_t outbound_payloads[128];
  size_t outbound_payload_count;
  uint32_t maximum_packet_bytes;
  uint64_t rebind_epoch;
  uint64_t remote_target_epoch;
  uint16_t highest_sequence;
  uint64_t sequence_window;
  int source_locked;
  int source_ssrc_learned;
  int rtcp_source_locked;
  int remote_target_set;
  int srtp_enabled;
  void *srtp_outbound;
  void *srtp_inbound;
  aula_rtp_srtp_direction_usage srtp_outbound_usage;
  aula_rtp_srtp_direction_usage srtp_inbound_usage;
  int sequence_initialized;
  int symmetric_rtp_enabled;
  int rtp_socket;
  int rtcp_socket;
};

uint16_t aula_rtp_read_u16(const uint8_t *data);
uint32_t aula_rtp_read_u32(const uint8_t *data);
int aula_rtp_address_is_valid(const uint8_t *address, uint8_t address_length);
int aula_rtp_source_is_valid(const aula_rtp_source *source);
int aula_rtp_remote_target_is_valid(const aula_rtp_remote_target *target);
int aula_rtp_payload_is_accepted(const aula_rtp_session *session, uint8_t payload_type);
int aula_rtp_payload_is_sendable(const aula_rtp_session *session,
                                  uint8_t payload_type);
int aula_rtp_mux_payloads_are_safe(const aula_rtp_session *session);
aula_status aula_rtp_policy_is_valid(const aula_rtp_session_policy *policy,
                                       const aula_rtp_identity *identity);
void aula_rtp_close_sockets(aula_rtp_session *session);
size_t aula_rtp_session_get_descriptors_internal(
    const aula_rtp_session *session, int *descriptors, size_t capacity);
aula_status aula_rtp_bind_pair(aula_rtp_session *session, const uint8_t *address,
                                 uint8_t address_length, uint16_t rtp_port, int rtcp_mux);
aula_status aula_rtp_session_init(const aula_rtp_identity *identity,
                                    const aula_rtp_session_policy *policy,
                                    aula_rtp_session **out_session);
void aula_rtp_account_sequence(aula_rtp_session *session, uint16_t sequence_number);
aula_status aula_rtp_source_from_sockaddr(const struct sockaddr_storage *address,
                                            socklen_t address_length,
                                            aula_rtp_source *out_source);
aula_status aula_rtp_send_to_address(int socket_fd, const uint8_t *target_address,
                                       uint8_t target_address_length, uint16_t target_port,
                                       aula_bytes packet);
aula_status aula_rtp_receive_datagram(int socket_fd, aula_mutable_bytes *packet_buffer,
                                        struct sockaddr_storage *out_address,
                                        socklen_t *out_address_length);
aula_status aula_rtp_srtp_configure(aula_rtp_session *session,
                                      const aula_rtp_srtp_config *config,
                                      const aula_rtp_srtp_limits *limits);
aula_status aula_rtp_srtp_protect_rtp(aula_rtp_session *session,
                                        aula_mutable_bytes *packet);
aula_status aula_rtp_srtp_unprotect_rtp(aula_rtp_session *session,
                                          aula_mutable_bytes *packet);
aula_status aula_rtp_srtp_protect_rtcp(aula_rtp_session *session,
                                         aula_mutable_bytes *packet);
aula_status aula_rtp_srtp_unprotect_rtcp(aula_rtp_session *session,
                                           aula_mutable_bytes *packet);
void aula_rtp_srtp_destroy(aula_rtp_session *session);

#endif
