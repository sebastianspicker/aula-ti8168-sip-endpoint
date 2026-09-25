#ifndef LS200_SIPD_RTP_SESSION_INTERNAL_H
#define LS200_SIPD_RTP_SESSION_INTERNAL_H

#include "ls200_sipd/rtp.h"
#include "srtp_private.h"

#include <sys/socket.h>

struct ls200_rtp_retransmit_cache;

struct ls200_rtp_session {
  struct ls200_rtp_retransmit_cache *retransmit;
  ls200_rtp_port_pair ports;
  ls200_rtp_identity identity;
  ls200_rtp_source source;
  ls200_rtp_source rtcp_source;
  ls200_rtp_remote_target remote_target;
  ls200_rtp_counters counters;
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
  ls200_rtp_srtp_direction_usage srtp_outbound_usage;
  ls200_rtp_srtp_direction_usage srtp_inbound_usage;
  int sequence_initialized;
  int symmetric_rtp_enabled;
  int rtp_socket;
  int rtcp_socket;
};

uint16_t ls200_rtp_read_u16(const uint8_t *data);
uint32_t ls200_rtp_read_u32(const uint8_t *data);
int ls200_rtp_address_is_valid(const uint8_t *address, uint8_t address_length);
int ls200_rtp_source_is_valid(const ls200_rtp_source *source);
int ls200_rtp_remote_target_is_valid(const ls200_rtp_remote_target *target);
int ls200_rtp_payload_is_accepted(const ls200_rtp_session *session, uint8_t payload_type);
int ls200_rtp_payload_is_sendable(const ls200_rtp_session *session,
                                  uint8_t payload_type);
int ls200_rtp_mux_payloads_are_safe(const ls200_rtp_session *session);
ls200_status ls200_rtp_policy_is_valid(const ls200_rtp_session_policy *policy,
                                       const ls200_rtp_identity *identity);
void ls200_rtp_close_sockets(ls200_rtp_session *session);
size_t ls200_rtp_session_get_descriptors_internal(
    const ls200_rtp_session *session, int *descriptors, size_t capacity);
ls200_status ls200_rtp_bind_pair(ls200_rtp_session *session, const uint8_t *address,
                                 uint8_t address_length, uint16_t rtp_port, int rtcp_mux);
ls200_status ls200_rtp_session_init(const ls200_rtp_identity *identity,
                                    const ls200_rtp_session_policy *policy,
                                    ls200_rtp_session **out_session);
void ls200_rtp_account_sequence(ls200_rtp_session *session, uint16_t sequence_number);
ls200_status ls200_rtp_source_from_sockaddr(const struct sockaddr_storage *address,
                                            socklen_t address_length,
                                            ls200_rtp_source *out_source);
ls200_status ls200_rtp_send_to_address(int socket_fd, const uint8_t *target_address,
                                       uint8_t target_address_length, uint16_t target_port,
                                       ls200_bytes packet);
ls200_status ls200_rtp_receive_datagram(int socket_fd, ls200_mutable_bytes *packet_buffer,
                                        struct sockaddr_storage *out_address,
                                        socklen_t *out_address_length);
ls200_status ls200_rtp_srtp_configure(ls200_rtp_session *session,
                                      const ls200_rtp_srtp_config *config,
                                      const ls200_rtp_srtp_limits *limits);
ls200_status ls200_rtp_srtp_protect_rtp(ls200_rtp_session *session,
                                        ls200_mutable_bytes *packet);
ls200_status ls200_rtp_srtp_unprotect_rtp(ls200_rtp_session *session,
                                          ls200_mutable_bytes *packet);
ls200_status ls200_rtp_srtp_protect_rtcp(ls200_rtp_session *session,
                                         ls200_mutable_bytes *packet);
ls200_status ls200_rtp_srtp_unprotect_rtcp(ls200_rtp_session *session,
                                           ls200_mutable_bytes *packet);
void ls200_rtp_srtp_destroy(ls200_rtp_session *session);

#endif
