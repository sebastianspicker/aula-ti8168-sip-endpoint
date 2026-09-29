#ifndef AULA_SIPD_RTP_H
#define AULA_SIPD_RTP_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_rtp_header {
  uint8_t payload_type;
  uint8_t marker;
  uint16_t sequence_number;
  uint32_t timestamp;
  uint32_t ssrc;
  uint16_t header_bytes;
} aula_rtp_header;

typedef struct aula_rtp_packet {
  aula_rtp_header header;
  aula_bytes payload;
} aula_rtp_packet;

typedef struct aula_rtp_source {
  uint32_t ssrc;
  uint16_t port;
  uint8_t address[16];
  uint8_t address_length;
} aula_rtp_source;

typedef struct aula_rtp_port_pair {
  uint16_t rtp_port;
  uint16_t rtcp_port;
  int rtcp_mux;
} aula_rtp_port_pair;

typedef struct aula_rtp_transport_config {
  const uint8_t *local_address;
  uint8_t local_address_length;
  uint16_t minimum_port;
  uint16_t maximum_port;
  int rtcp_mux;
} aula_rtp_transport_config;

typedef struct aula_rtp_transport_info {
  aula_rtp_port_pair ports;
  uint8_t local_address[16];
  uint8_t local_address_length;
} aula_rtp_transport_info;

typedef struct aula_rtp_remote_target {
  uint8_t rtp_address[16];
  uint8_t rtp_address_length;
  uint16_t rtp_port;
  uint8_t rtcp_address[16];
  uint8_t rtcp_address_length;
  uint16_t rtcp_port;
  int rtcp_mux;
} aula_rtp_remote_target;

typedef struct aula_rtp_identity {
  uint32_t ssrc;
  uint16_t initial_sequence_number;
  uint32_t initial_timestamp;
  uint8_t payload_type;
} aula_rtp_identity;

#define AULA_RTP_SRTP_MASTER_KEY_SALT_BYTES 30U
#define AULA_RTP_SRTP_RTP_OVERHEAD_BYTES 10U
#define AULA_RTP_SRTP_RTCP_OVERHEAD_BYTES 14U

/* Binary keying material only. SDP inline syntax never crosses this API. */
typedef struct aula_rtp_srtp_config {
  uint8_t local_outbound_key_salt[AULA_RTP_SRTP_MASTER_KEY_SALT_BYTES];
  uint8_t remote_inbound_key_salt[AULA_RTP_SRTP_MASTER_KEY_SALT_BYTES];
} aula_rtp_srtp_config;

typedef struct aula_rtp_counters {
  uint64_t packets;
  uint64_t bytes;
  uint64_t lost;
  uint64_t duplicates;
  uint64_t reordered;
  uint64_t rejected;
  uint64_t jitter_ns;
} aula_rtp_counters;

typedef struct aula_rtp_payload_set {
  const uint8_t *payload_types;
  size_t payload_type_count;
} aula_rtp_payload_set;

typedef struct aula_rtp_peer_policy {
  const uint8_t *bind_address;
  uint8_t bind_address_length;
  const aula_rtp_source *expected_peer;
  int expected_peer_authenticated;
  int symmetric_rtp_enabled;
  const aula_rtp_remote_target *negotiated_remote;
  int negotiated_remote_authenticated;
} aula_rtp_peer_policy;

typedef struct aula_rtp_session_policy {
  aula_rtp_payload_set accepted_payloads;
  aula_rtp_peer_policy peer;
  uint32_t maximum_packet_bytes;
} aula_rtp_session_policy;

typedef struct aula_rtp_rebind_authorization {
  aula_rtp_source approved_peer;
  uint64_t authorization_epoch;
  const char *reason_code;
} aula_rtp_rebind_authorization;

typedef struct aula_rtp_remote_target_authorization {
  aula_rtp_remote_target approved_remote;
  uint64_t authorization_epoch;
  const char *reason_code;
} aula_rtp_remote_target_authorization;

typedef struct aula_rtp_pacing_config {
  uint64_t packet_interval_ns;
  uint32_t maximum_queue_packets;
  uint32_t maximum_queue_bytes;
} aula_rtp_pacing_config;

typedef struct aula_rtp_send_queue_stats {
  uint32_t queued_packets;
  uint32_t queued_bytes;
  uint64_t sent_packets;
  uint64_t dropped_packets;
  uint64_t next_send_ns;
} aula_rtp_send_queue_stats;

typedef struct aula_rtp_session aula_rtp_session;
typedef struct aula_rtp_send_queue aula_rtp_send_queue;

aula_status aula_rtp_parse(aula_bytes input, aula_rtp_packet *out_packet);
aula_status aula_rtp_serialize(const aula_rtp_packet *packet,
                                 aula_mutable_bytes *output);
aula_status aula_rtp_validate_source(const aula_rtp_source *locked_source,
                                       const aula_rtp_source *candidate,
                                       int symmetric_rtp_enabled);
aula_status aula_rtp_identity_create(aula_bytes random_bytes,
                                       uint8_t payload_type,
                                       aula_rtp_identity *out_identity);
aula_status aula_rtp_session_create(const aula_rtp_port_pair *ports,
                                      const aula_rtp_identity *identity,
                                      aula_rtp_session **out_session);
aula_status aula_rtp_session_create_with_policy(const aula_rtp_port_pair *ports,
                                                  const aula_rtp_identity *identity,
                                                  const aula_rtp_session_policy *policy,
                                                  aula_rtp_session **out_session);
/*
 * Atomically searches and retains an RTP/RTCP reservation.  The returned
 * session owns the bound sockets until aula_rtp_session_destroy(); callers
 * must obtain ports from aula_rtp_session_get_transport() and never from a
 * detached probe or allocator.
 */
aula_status aula_rtp_session_reserve(const aula_rtp_transport_config *transport,
                                       const aula_rtp_identity *identity,
                                       const aula_rtp_session_policy *policy,
                                       aula_rtp_session **out_session);
aula_status aula_rtp_session_get_transport(const aula_rtp_session *session,
                                             aula_rtp_transport_info *out_transport);
aula_status aula_rtp_session_accept(aula_rtp_session *session,
                                      const aula_rtp_source *source,
                                      aula_bytes packet,
                                      int symmetric_rtp_enabled,
                                      aula_rtp_packet *out_packet);
aula_status aula_rtp_session_get_counters(const aula_rtp_session *session,
                                            aula_rtp_counters *out_counters);
/* Rebinding requires authorization from the validated signaling path. */
aula_status aula_rtp_session_authorize_rebind(aula_rtp_session *session,
                                                const aula_rtp_rebind_authorization *authorization);
aula_status aula_rtp_session_authorize_remote_target(
    aula_rtp_session *session,
    const aula_rtp_remote_target_authorization *authorization);
/* Configures AES_CM_128_HMAC_SHA1_80 contexts after the remote target has
 * been authorized. Unsupported builds leave the session plaintext-only. */
aula_status aula_rtp_session_configure_srtp(
    aula_rtp_session *session, const aula_rtp_srtp_config *config);
aula_status aula_rtp_session_poll(const aula_rtp_session *session,
                                    int *out_rtp_ready, int *out_rtcp_ready);
aula_status aula_rtp_session_receive_rtp(aula_rtp_session *session,
                                           aula_mutable_bytes *packet_buffer,
                                           aula_rtp_source *out_source,
                                           aula_rtp_packet *out_packet);
aula_status aula_rtp_session_receive_rtcp(aula_rtp_session *session,
                                            aula_mutable_bytes *packet_buffer,
                                            aula_rtp_source *out_source);
aula_status aula_rtp_session_send_rtp(aula_rtp_session *session,
                                        const aula_rtp_packet *packet);
aula_status aula_rtp_session_send_rtcp(aula_rtp_session *session,
                                         aula_bytes packet);
void aula_rtp_session_destroy(aula_rtp_session *session);
aula_status aula_rtp_send_queue_create(const aula_rtp_pacing_config *config,
                                         aula_rtp_send_queue **out_queue);
aula_status aula_rtp_send_queue_enqueue(aula_rtp_send_queue *queue,
                                          const aula_rtp_packet *packet,
                                          uint64_t now_ns);
aula_status aula_rtp_send_queue_dequeue(aula_rtp_send_queue *queue,
                                          uint64_t now_ns,
                                          aula_rtp_packet *out_packet);
aula_status aula_rtp_send_queue_get_stats(const aula_rtp_send_queue *queue,
                                            aula_rtp_send_queue_stats *out_stats);
/* Drops every queued packet without transferring it to a transport. */
aula_status aula_rtp_send_queue_discard_all(aula_rtp_send_queue *queue);
void aula_rtp_send_queue_destroy(aula_rtp_send_queue *queue);

#ifdef __cplusplus
}
#endif

#endif
