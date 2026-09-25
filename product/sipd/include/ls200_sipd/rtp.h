#ifndef LS200_SIPD_RTP_H
#define LS200_SIPD_RTP_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_rtp_header {
  uint8_t payload_type;
  uint8_t marker;
  uint16_t sequence_number;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t header_bytes;
} ls200_rtp_header;

typedef struct ls200_rtp_packet {
  ls200_rtp_header header;
  ls200_bytes payload;
} ls200_rtp_packet;

typedef struct ls200_rtp_source {
  uint32_t ssrc;
  uint16_t port;
  uint8_t address[16];
  uint8_t address_length;
} ls200_rtp_source;

typedef struct ls200_rtp_port_pair {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  int rtcp_mux;
} ls200_rtp_port_pair;

typedef struct ls200_rtp_transport_config {
  const uint8_t *local_address;
  uint8_t local_address_length;
  uint16_t minimum_port;
  uint16_t maximum_port;
  int rtcp_mux;
} ls200_rtp_transport_config;

typedef struct ls200_rtp_transport_info {
  ls200_rtp_port_pair ports;
  uint8_t local_address[16];
  uint8_t local_address_length;
} ls200_rtp_transport_info;

typedef struct ls200_rtp_remote_target {
  uint8_t rtp_address[16];
  uint8_t rtp_address_length;
  uint16_t rtp_port;
  uint8_t rtcp_address[16];
  uint8_t rtcp_address_length;
  uint16_t rtcp_port;
  int rtcp_mux;
} ls200_rtp_remote_target;

typedef struct ls200_rtp_identity {
  uint32_t ssrc;
  uint16_t initial_sequence_number;
  uint32_t initial_timestamp;
  uint8_t payload_type;
} ls200_rtp_identity;

#define LS200_RTP_SRTP_MASTER_KEY_SALT_BYTES 30U
#define LS200_RTP_SRTP_RTP_OVERHEAD_BYTES 10U
#define LS200_RTP_SRTP_RTCP_OVERHEAD_BYTES 14U

/* Binary keying material only. SDP inline syntax never crosses this API. */
typedef struct ls200_rtp_srtp_config {
  uint8_t local_outbound_key_salt[LS200_RTP_SRTP_MASTER_KEY_SALT_BYTES];
  uint8_t remote_inbound_key_salt[LS200_RTP_SRTP_MASTER_KEY_SALT_BYTES];
} ls200_rtp_srtp_config;

typedef struct ls200_rtp_counters {
  uint64_t packets;
  uint64_t bytes;
  uint64_t lost;
  uint64_t duplicates;
  uint64_t reordered;
  uint64_t rejected;
  uint64_t jitter_ns;
} ls200_rtp_counters;

typedef struct ls200_rtp_payload_set {
  const uint8_t *payload_types;
  size_t payload_type_count;
} ls200_rtp_payload_set;

typedef struct ls200_rtp_peer_policy {
  const uint8_t *bind_address;
  uint8_t bind_address_length;
  const ls200_rtp_source *expected_peer;
  int expected_peer_authenticated;
  int symmetric_rtp_enabled;
  const ls200_rtp_remote_target *negotiated_remote;
  int negotiated_remote_authenticated;
} ls200_rtp_peer_policy;

typedef struct ls200_rtp_session_policy {
  ls200_rtp_payload_set accepted_payloads;
  ls200_rtp_peer_policy peer;
  uint32_t maximum_packet_bytes;
} ls200_rtp_session_policy;

typedef struct ls200_rtp_rebind_authorization {
  ls200_rtp_source approved_peer;
  uint64_t authorization_epoch;
  const char *reason_code;
} ls200_rtp_rebind_authorization;

typedef struct ls200_rtp_remote_target_authorization {
  ls200_rtp_remote_target approved_remote;
  uint64_t authorization_epoch;
  const char *reason_code;
} ls200_rtp_remote_target_authorization;

typedef struct ls200_rtp_pacing_config {
  uint64_t packet_interval_ns;
  uint32_t maximum_queue_packets;
  uint32_t maximum_queue_bytes;
} ls200_rtp_pacing_config;

typedef struct ls200_rtp_send_queue_stats {
  uint32_t queued_packets;
  uint32_t queued_bytes;
  uint64_t sent_packets;
  uint64_t dropped_packets;
  uint64_t next_send_ns;
} ls200_rtp_send_queue_stats;

typedef struct ls200_rtp_session ls200_rtp_session;
typedef struct ls200_rtp_send_queue ls200_rtp_send_queue;

ls200_status ls200_rtp_parse(ls200_bytes input, ls200_rtp_packet *out_packet);
ls200_status ls200_rtp_serialize(const ls200_rtp_packet *packet,
                                 ls200_mutable_bytes *output);
ls200_status ls200_rtp_validate_source(const ls200_rtp_source *locked_source,
                                       const ls200_rtp_source *candidate,
                                       int symmetric_rtp_enabled);
ls200_status ls200_rtp_identity_create(ls200_bytes random_bytes,
                                       uint8_t payload_type,
                                       ls200_rtp_identity *out_identity);
ls200_status ls200_rtp_session_create(const ls200_rtp_port_pair *ports,
                                      const ls200_rtp_identity *identity,
                                      ls200_rtp_session **out_session);
ls200_status ls200_rtp_session_create_with_policy(const ls200_rtp_port_pair *ports,
                                                  const ls200_rtp_identity *identity,
                                                  const ls200_rtp_session_policy *policy,
                                                  ls200_rtp_session **out_session);
/*
 * Atomically searches and retains an RTP/RTCP reservation.  The returned
 * session owns the bound sockets until ls200_rtp_session_destroy(); callers
 * must obtain ports from ls200_rtp_session_get_transport() and never from a
 * detached probe or allocator.
 */
ls200_status ls200_rtp_session_reserve(const ls200_rtp_transport_config *transport,
                                       const ls200_rtp_identity *identity,
                                       const ls200_rtp_session_policy *policy,
                                       ls200_rtp_session **out_session);
ls200_status ls200_rtp_session_get_transport(const ls200_rtp_session *session,
                                             ls200_rtp_transport_info *out_transport);
ls200_status ls200_rtp_session_accept(ls200_rtp_session *session,
                                      const ls200_rtp_source *source,
                                      ls200_bytes packet,
                                      int symmetric_rtp_enabled,
                                      ls200_rtp_packet *out_packet);
ls200_status ls200_rtp_session_get_counters(const ls200_rtp_session *session,
                                            ls200_rtp_counters *out_counters);
/* Rebinding requires authorization from the validated signaling path. */
ls200_status ls200_rtp_session_authorize_rebind(ls200_rtp_session *session,
                                                const ls200_rtp_rebind_authorization *authorization);
ls200_status ls200_rtp_session_authorize_remote_target(
    ls200_rtp_session *session,
    const ls200_rtp_remote_target_authorization *authorization);
/* Configures AES_CM_128_HMAC_SHA1_80 contexts after the remote target has
 * been authorized. Unsupported builds leave the session plaintext-only. */
ls200_status ls200_rtp_session_configure_srtp(
    ls200_rtp_session *session, const ls200_rtp_srtp_config *config);
ls200_status ls200_rtp_session_poll(const ls200_rtp_session *session,
                                    int *out_rtp_ready, int *out_rtcp_ready);
ls200_status ls200_rtp_session_receive_rtp(ls200_rtp_session *session,
                                           ls200_mutable_bytes *packet_buffer,
                                           ls200_rtp_source *out_source,
                                           ls200_rtp_packet *out_packet);
ls200_status ls200_rtp_session_receive_rtcp(ls200_rtp_session *session,
                                            ls200_mutable_bytes *packet_buffer,
                                            ls200_rtp_source *out_source);
ls200_status ls200_rtp_session_send_rtp(ls200_rtp_session *session,
                                        const ls200_rtp_packet *packet);
ls200_status ls200_rtp_session_send_rtcp(ls200_rtp_session *session,
                                         ls200_bytes packet);
void ls200_rtp_session_destroy(ls200_rtp_session *session);
ls200_status ls200_rtp_send_queue_create(const ls200_rtp_pacing_config *config,
                                         ls200_rtp_send_queue **out_queue);
ls200_status ls200_rtp_send_queue_enqueue(ls200_rtp_send_queue *queue,
                                          const ls200_rtp_packet *packet,
                                          uint64_t now_ns);
ls200_status ls200_rtp_send_queue_dequeue(ls200_rtp_send_queue *queue,
                                          uint64_t now_ns,
                                          ls200_rtp_packet *out_packet);
ls200_status ls200_rtp_send_queue_get_stats(const ls200_rtp_send_queue *queue,
                                            ls200_rtp_send_queue_stats *out_stats);
/* Drops every queued packet without transferring it to a transport. */
ls200_status ls200_rtp_send_queue_discard_all(ls200_rtp_send_queue *queue);
void ls200_rtp_send_queue_destroy(ls200_rtp_send_queue *queue);

#ifdef __cplusplus
}
#endif

#endif
