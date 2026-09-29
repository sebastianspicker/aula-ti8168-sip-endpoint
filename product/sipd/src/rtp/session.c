#include "session_internal.h"
#include "retransmit_private.h"
#include "aula_sipd/platform.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#include "wire.h"

aula_status aula_rtp_parse(aula_bytes input, aula_rtp_packet *out_packet) {
  return aula_rtp_wire_parse(input, out_packet);
}

aula_status aula_rtp_serialize(const aula_rtp_packet *packet, aula_mutable_bytes *output) {
  return aula_rtp_wire_serialize(packet, output);
}

aula_status aula_rtp_validate_source(const aula_rtp_source *locked_source,
                                       const aula_rtp_source *candidate,
                                       int symmetric_rtp_enabled) {
  (void)symmetric_rtp_enabled;
  if (!aula_rtp_source_is_valid(locked_source) || !aula_rtp_source_is_valid(candidate)) return AULA_STATUS_INVALID_ARGUMENT;
  if (locked_source->ssrc != candidate->ssrc || locked_source->port != candidate->port ||
      locked_source->address_length != candidate->address_length ||
      memcmp(locked_source->address, candidate->address, locked_source->address_length) != 0) return AULA_STATUS_SECURITY_ERROR;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_identity_create(aula_bytes random_bytes, uint8_t payload_type,
                                       aula_rtp_identity *out_identity) {
  if (out_identity == NULL || random_bytes.data == NULL || random_bytes.length < 10U || payload_type > 127U) return AULA_STATUS_INVALID_ARGUMENT;
  out_identity->ssrc = aula_rtp_read_u32(random_bytes.data);
  out_identity->initial_sequence_number = aula_rtp_read_u16(random_bytes.data + 4U);
  out_identity->initial_timestamp = aula_rtp_read_u32(random_bytes.data + 6U);
  out_identity->payload_type = payload_type;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_create(const aula_rtp_port_pair *ports,
                                      const aula_rtp_identity *identity,
                                      aula_rtp_session **out_session) {
  uint8_t payload_type = identity == NULL ? 0U : identity->payload_type;
  uint8_t loopback[4] = {127U, 0U, 0U, 1U};
  aula_rtp_session_policy policy;
  (void)memset(&policy, 0, sizeof(policy));
  policy.accepted_payloads.payload_types = &payload_type;
  policy.accepted_payloads.payload_type_count = 1U;
  policy.peer.bind_address = loopback;
  policy.peer.bind_address_length = 4U;
  policy.maximum_packet_bytes = AULA_SIPD_MAX_RTP_PACKET_BYTES;
  return aula_rtp_session_create_with_policy(ports, identity, &policy, out_session);
}

aula_status aula_rtp_session_create_with_policy(const aula_rtp_port_pair *ports,
                                                  const aula_rtp_identity *identity,
                                                  const aula_rtp_session_policy *policy,
                                                  aula_rtp_session **out_session) {
  uint8_t loopback[4] = {127U, 0U, 0U, 1U};
  const uint8_t *local_address;
  uint8_t local_address_length;
  aula_rtp_session *session;
  aula_status status;
  if (ports == NULL || ports->rtp_port == 0U || (ports->rtp_port & 1U) != 0U ||
      (!ports->rtcp_mux && (ports->rtp_port == UINT16_MAX || ports->rtcp_port != (uint16_t)(ports->rtp_port + 1U))) ||
      (ports->rtcp_mux && ports->rtcp_port != ports->rtp_port)) return AULA_STATUS_INVALID_ARGUMENT;
  local_address = policy != NULL && policy->peer.bind_address_length != 0U ? policy->peer.bind_address : loopback;
  local_address_length = policy != NULL && policy->peer.bind_address_length != 0U ? policy->peer.bind_address_length : 4U;
  status = aula_rtp_session_init(identity, policy, &session);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_bind_pair(session, local_address, local_address_length, ports->rtp_port, ports->rtcp_mux);
  if (status != AULA_STATUS_OK) {
    aula_rtp_srtp_destroy(session);
    free(session);
    return status;
  }
  (void)memcpy(session->local_address, local_address, local_address_length);
  session->local_address_length = local_address_length;
  *out_session = session;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_reserve(const aula_rtp_transport_config *transport,
                                       const aula_rtp_identity *identity,
                                       const aula_rtp_session_policy *policy,
                                       aula_rtp_session **out_session) {
  uint32_t candidate;
  aula_rtp_session *session;
  aula_status status;
  if (transport == NULL || out_session == NULL || transport->minimum_port == 0U ||
      transport->maximum_port < transport->minimum_port ||
      !aula_rtp_address_is_valid(transport->local_address, transport->local_address_length)) return AULA_STATUS_INVALID_ARGUMENT;
  *out_session = NULL;
  status = aula_rtp_session_init(identity, policy, &session);
  if (status != AULA_STATUS_OK) return status;
  candidate = (uint32_t)transport->minimum_port + ((uint32_t)transport->minimum_port & 1U);
  for (; candidate <= transport->maximum_port; candidate += 2U) {
    if ((!transport->rtcp_mux && candidate == UINT16_MAX) ||
        (!transport->rtcp_mux && candidate + 1U > transport->maximum_port)) break;
    status = aula_rtp_bind_pair(session, transport->local_address, transport->local_address_length,
                                 (uint16_t)candidate, transport->rtcp_mux);
    if (status == AULA_STATUS_OK) {
      (void)memcpy(session->local_address, transport->local_address, transport->local_address_length);
      session->local_address_length = transport->local_address_length;
      *out_session = session;
      return AULA_STATUS_OK;
    }
  }
  aula_rtp_srtp_destroy(session);
  free(session);
  return AULA_STATUS_IO_ERROR;
}

aula_status aula_rtp_session_get_transport(const aula_rtp_session *session,
                                             aula_rtp_transport_info *out_transport) {
  if (session == NULL || out_transport == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_transport, 0, sizeof(*out_transport));
  out_transport->ports = session->ports;
  (void)memcpy(out_transport->local_address, session->local_address, session->local_address_length);
  out_transport->local_address_length = session->local_address_length;
  return AULA_STATUS_OK;
}

static aula_status aula_rtp_accept_source(aula_rtp_session *session,
                                            const aula_rtp_source *source,
                                            uint32_t ssrc) {
  aula_rtp_source observed = *source;
  aula_status status;
  observed.ssrc = ssrc;
  if (!session->source_locked) {
    session->source = observed;
    session->source_locked = 1;
    session->source_ssrc_learned = 1;
    return AULA_STATUS_OK;
  }
  status = session->source_ssrc_learned ?
      aula_rtp_validate_source(&session->source, &observed, 0) :
      (session->source.address_length == observed.address_length &&
       session->source.port == observed.port &&
       memcmp(session->source.address, observed.address,
              session->source.address_length) == 0 ?
          AULA_STATUS_OK : AULA_STATUS_SECURITY_ERROR);
  if (status == AULA_STATUS_OK && !session->source_ssrc_learned) {
    session->source.ssrc = observed.ssrc;
    session->source_ssrc_learned = 1;
  }
  return status;
}

aula_status aula_rtp_session_accept(aula_rtp_session *session, const aula_rtp_source *source,
                                      aula_bytes packet, int symmetric_rtp_enabled,
                                      aula_rtp_packet *out_packet) {
  aula_rtp_packet parsed;
  aula_status status;
  if (session == NULL || out_packet == NULL || !aula_rtp_source_is_valid(source)) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_rtp_parse(packet, &parsed);
  if (status != AULA_STATUS_OK) {
    session->counters.rejected++;
    return status;
  }
  if (packet.length > session->maximum_packet_bytes || !aula_rtp_payload_is_accepted(session, parsed.header.payload_type)) {
    session->counters.rejected++;
    return AULA_STATUS_INVALID_DATA;
  }
  if (parsed.header.ssrc == session->identity.ssrc) {
    session->counters.rejected++;
    return AULA_STATUS_SECURITY_ERROR;
  }
  if (symmetric_rtp_enabled != 0 && !session->symmetric_rtp_enabled) {
    session->counters.rejected++;
    return AULA_STATUS_PERMISSION_DENIED;
  }
  status = aula_rtp_accept_source(session, source, parsed.header.ssrc);
  if (status != AULA_STATUS_OK) {
    session->counters.rejected++;
    return status;
  }
  aula_rtp_account_sequence(session, parsed.header.sequence_number);
  session->counters.bytes += parsed.payload.length;
  *out_packet = parsed;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_authorize_rebind(aula_rtp_session *session,
                                                const aula_rtp_rebind_authorization *authorization) {
  const aula_rtp_source *peer;
  if (session == NULL || authorization == NULL || authorization->reason_code == NULL ||
      authorization->reason_code[0] == '\0' || !aula_rtp_source_is_valid(&authorization->approved_peer)) return AULA_STATUS_INVALID_ARGUMENT;
  peer = &authorization->approved_peer;
  if (!session->source_locked || !session->symmetric_rtp_enabled ||
      authorization->authorization_epoch <= session->rebind_epoch || peer->ssrc == session->identity.ssrc) return AULA_STATUS_PERMISSION_DENIED;
  session->source = *peer;
  session->source_ssrc_learned = 1;
  session->rebind_epoch = authorization->authorization_epoch;
  session->sequence_initialized = 0;
  session->sequence_window = UINT64_C(0);
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_authorize_remote_target(
    aula_rtp_session *session,
    const aula_rtp_remote_target_authorization *authorization) {
  if (session == NULL || authorization == NULL || authorization->reason_code == NULL ||
      authorization->reason_code[0] == '\0' ||
      !aula_rtp_remote_target_is_valid(&authorization->approved_remote)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (authorization->authorization_epoch <= session->remote_target_epoch) {
    return AULA_STATUS_PERMISSION_DENIED;
  }
  aula_rtp_retransmit_reset(session);
  session->remote_target = authorization->approved_remote;
  (void)memset(&session->source, 0, sizeof(session->source));
  (void)memcpy(session->source.address, authorization->approved_remote.rtp_address,
               authorization->approved_remote.rtp_address_length);
  session->source.address_length = authorization->approved_remote.rtp_address_length;
  session->source.port = authorization->approved_remote.rtp_port;
  session->source_locked = 1;
  session->source_ssrc_learned = 0;
  (void)memset(&session->rtcp_source, 0, sizeof(session->rtcp_source));
  (void)memcpy(session->rtcp_source.address, authorization->approved_remote.rtcp_address,
               authorization->approved_remote.rtcp_address_length);
  session->rtcp_source.address_length = authorization->approved_remote.rtcp_address_length;
  session->rtcp_source.port = authorization->approved_remote.rtcp_port;
  session->rtcp_source_locked = 1;
  session->remote_target_epoch = authorization->authorization_epoch;
  session->remote_target_set = 1;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_configure_srtp(
    aula_rtp_session *session, const aula_rtp_srtp_config *config) {
  const aula_rtp_srtp_limits limits = {
      AULA_RTP_SRTP_MAX_RTP_LIFETIME, AULA_RTP_SRTP_MAX_RTP_LIFETIME};
  return aula_rtp_srtp_configure(session, config, &limits);
}

aula_status aula_rtp_session_configure_srtp_with_limits(
    aula_rtp_session *session, const aula_rtp_srtp_config *config,
    const aula_rtp_srtp_limits *limits) {
  return aula_rtp_srtp_configure(session, config, limits);
}

aula_status aula_rtp_session_get_counters(const aula_rtp_session *session, aula_rtp_counters *out_counters) {
  if (session == NULL || out_counters == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_counters = session->counters;
  return AULA_STATUS_OK;
}

static int aula_rtp_poll_has_error(const struct pollfd *descriptors,
                                    int rtcp_mux) {
  if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return 1;
  return !rtcp_mux && (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

static aula_status aula_rtp_classify_mux_poll(const aula_rtp_session *session,
                                                int *out_rtp_ready, int *out_rtcp_ready) {
  uint8_t header[2];
  ssize_t peeked;
  if (!session->ports.rtcp_mux || *out_rtp_ready == 0) return AULA_STATUS_OK;
  peeked = recv(session->rtp_socket, header, sizeof(header), MSG_PEEK);
  if (peeked < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
        AULA_STATUS_AGAIN : AULA_STATUS_IO_ERROR;
  }
  if (peeked >= (ssize_t)sizeof(header) && header[1] >= 192U && header[1] <= 223U) {
    *out_rtp_ready = 0;
    *out_rtcp_ready = 1;
  }
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_poll(const aula_rtp_session *session, int *out_rtp_ready, int *out_rtcp_ready) {
  struct pollfd descriptors[2];
  nfds_t descriptor_count = 1U;
  int result;
  aula_status status;
  if (session == NULL || out_rtp_ready == NULL || out_rtcp_ready == NULL || session->rtp_socket < 0) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(descriptors, 0, sizeof(descriptors));
  descriptors[0].fd = session->rtp_socket;
  descriptors[0].events = POLLIN;
  if (!session->ports.rtcp_mux) {
    if (session->rtcp_socket < 0) return AULA_STATUS_STATE_ERROR;
    descriptors[1].fd = session->rtcp_socket;
    descriptors[1].events = POLLIN;
    descriptor_count = 2U;
  }
  result = poll(descriptors, descriptor_count, 0);
  if (result < 0) return errno == EINTR ? AULA_STATUS_AGAIN : AULA_STATUS_IO_ERROR;
  if (aula_rtp_poll_has_error(descriptors, session->ports.rtcp_mux)) {
    return AULA_STATUS_IO_ERROR;
  }
  *out_rtp_ready = (descriptors[0].revents & POLLIN) != 0;
  *out_rtcp_ready = session->ports.rtcp_mux ? 0 : ((descriptors[1].revents & POLLIN) != 0);
  status = aula_rtp_classify_mux_poll(session, out_rtp_ready, out_rtcp_ready);
  if (status != AULA_STATUS_OK) return status;
  return result == 0 ? AULA_STATUS_AGAIN : AULA_STATUS_OK;
}

static int aula_rtp_receive_rtp_arguments_are_valid(
    const aula_rtp_session *session, const aula_mutable_bytes *packet_buffer,
    const aula_rtp_source *out_source, const aula_rtp_packet *out_packet) {
  return session != NULL && packet_buffer != NULL && out_source != NULL &&
         out_packet != NULL && packet_buffer->data != NULL &&
         packet_buffer->capacity != 0U &&
         packet_buffer->capacity <= session->maximum_packet_bytes &&
         session->rtp_socket >= 0;
}

static int aula_rtp_receive_rtcp_arguments_are_valid(
    const aula_rtp_session *session, const aula_mutable_bytes *packet_buffer,
    const aula_rtp_source *out_source) {
  return session != NULL && packet_buffer != NULL && out_source != NULL &&
         packet_buffer->data != NULL && packet_buffer->capacity != 0U &&
         packet_buffer->capacity <= AULA_SIPD_MAX_RTCP_PACKET_BYTES;
}

static aula_status aula_rtp_unprotect_received_packet(
    aula_rtp_session *session, aula_mutable_bytes *protected_buffer,
    aula_mutable_bytes *packet_buffer, uint8_t *protected_packet,
    size_t protected_packet_size, int is_rtcp) {
  aula_status status;
  status = is_rtcp ? aula_rtp_srtp_unprotect_rtcp(session, protected_buffer) :
                     aula_rtp_srtp_unprotect_rtp(session, protected_buffer);
  if (status == AULA_STATUS_OK && protected_buffer->length > packet_buffer->capacity)
    status = AULA_STATUS_LIMIT_EXCEEDED;
  if (status == AULA_STATUS_OK) {
    (void)memcpy(packet_buffer->data, protected_buffer->data, protected_buffer->length);
    packet_buffer->length = protected_buffer->length;
  } else {
    session->counters.rejected++;
  }
  (void)memset(protected_packet, 0, protected_packet_size);
  return status;
}

static int aula_rtp_rtcp_source_is_authorized(
    aula_rtp_session *session, const aula_rtp_source *source) {
  if (session->rtcp_source_locked == 0) return 1;
  if (source->address_length != session->rtcp_source.address_length ||
      source->port != session->rtcp_source.port ||
      memcmp(source->address, session->rtcp_source.address,
             source->address_length) != 0) {
    session->counters.rejected++;
    return 0;
  }
  return 1;
}

/* SRTP authentication does not authorize a new network tuple.  Check the
 * numeric tuple before libsrtp sees a protected packet, otherwise an
 * off-tuple replay can consume the inbound replay window before it is
 * rejected below.  SSRC remains deliberately unlearned until the protected
 * RTP payload has authenticated and parsed successfully. */
static int aula_rtp_rtp_source_is_authorized(
    aula_rtp_session *session, const aula_rtp_source *source) {
  if (session->source_locked == 0) return 1;
  if (source->address_length != session->source.address_length ||
      source->port != session->source.port ||
      memcmp(source->address, session->source.address,
             source->address_length) != 0) {
    session->counters.rejected++;
    return 0;
  }
  return 1;
}

aula_status aula_rtp_session_receive_rtp(aula_rtp_session *session, aula_mutable_bytes *packet_buffer,
                                           aula_rtp_source *out_source, aula_rtp_packet *out_packet) {
  struct sockaddr_storage address;
  socklen_t address_length = (socklen_t)sizeof(address);
  aula_rtp_source source;
  uint8_t protected_packet[AULA_SIPD_MAX_RTP_PACKET_BYTES + AULA_RTP_SRTP_RTP_OVERHEAD_BYTES];
  aula_mutable_bytes protected_buffer = {protected_packet, sizeof(protected_packet), 0U};
  aula_status status;
  if (!aula_rtp_receive_rtp_arguments_are_valid(session, packet_buffer,
                                                 out_source, out_packet))
    return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_rtp_receive_datagram(session->rtp_socket,
                                      session->srtp_enabled != 0 ? &protected_buffer : packet_buffer,
                                      &address, &address_length);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_source_from_sockaddr(&address, address_length, &source);
  if (status != AULA_STATUS_OK) {
    session->counters.rejected++;
    (void)memset(protected_packet, 0, sizeof(protected_packet));
    return status;
  }
  if (session->srtp_enabled != 0) {
    if (!aula_rtp_rtp_source_is_authorized(session, &source)) {
      (void)memset(protected_packet, 0, sizeof(protected_packet));
      return AULA_STATUS_SECURITY_ERROR;
    }
    status = aula_rtp_unprotect_received_packet(session, &protected_buffer,
                                                  packet_buffer, protected_packet,
                                                  sizeof(protected_packet), 0);
    if (status != AULA_STATUS_OK) return status;
  }
  status = aula_rtp_session_accept(session, &source, (aula_bytes){packet_buffer->data, packet_buffer->length}, 0, out_packet);
  if (status == AULA_STATUS_OK) {
    source.ssrc = out_packet->header.ssrc;
    *out_source = source;
  }
  return status;
}

aula_status aula_rtp_session_receive_rtcp(aula_rtp_session *session, aula_mutable_bytes *packet_buffer,
                                            aula_rtp_source *out_source) {
  struct sockaddr_storage address;
  socklen_t address_length = (socklen_t)sizeof(address);
  int socket_fd;
  uint8_t protected_packet[AULA_SIPD_MAX_RTCP_PACKET_BYTES + AULA_RTP_SRTP_RTCP_OVERHEAD_BYTES];
  aula_mutable_bytes protected_buffer = {protected_packet, sizeof(protected_packet), 0U};
  aula_status status;
  if (!aula_rtp_receive_rtcp_arguments_are_valid(session, packet_buffer, out_source))
    return AULA_STATUS_INVALID_ARGUMENT;
  socket_fd = session->ports.rtcp_mux ? session->rtp_socket : session->rtcp_socket;
  if (socket_fd < 0) return AULA_STATUS_STATE_ERROR;
  status = aula_rtp_receive_datagram(socket_fd,
                                      session->srtp_enabled != 0 ? &protected_buffer : packet_buffer,
                                      &address, &address_length);
  if (status != AULA_STATUS_OK) return status;
  status = aula_rtp_source_from_sockaddr(&address, address_length, out_source);
  if (status != AULA_STATUS_OK) {
    session->counters.rejected++;
    (void)memset(protected_packet, 0, sizeof(protected_packet));
    return status;
  }
  if (session->srtp_enabled != 0) {
    if (!aula_rtp_rtcp_source_is_authorized(session, out_source)) {
      (void)memset(protected_packet, 0, sizeof(protected_packet));
      return AULA_STATUS_SECURITY_ERROR;
    }
    status = aula_rtp_unprotect_received_packet(session, &protected_buffer,
                                                  packet_buffer, protected_packet,
                                                  sizeof(protected_packet), 1);
    if (status != AULA_STATUS_OK) return status;
  }
  if (session->srtp_enabled == 0 && !aula_rtp_rtcp_source_is_authorized(session, out_source))
    return AULA_STATUS_SECURITY_ERROR;
  out_source->ssrc = 0U;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_send_rtp(aula_rtp_session *session, const aula_rtp_packet *packet) {
  uint8_t plaintext[AULA_SIPD_MAX_RTP_PACKET_BYTES];
  uint8_t wire[AULA_SIPD_MAX_RTP_PACKET_BYTES + AULA_RTP_SRTP_RTP_OVERHEAD_BYTES];
  aula_mutable_bytes output = {plaintext, sizeof(plaintext), 0U};
  aula_mutable_bytes protected_output = {wire, sizeof(wire), 0U};
  aula_status status;
  if (session == NULL || packet == NULL || !session->remote_target_set || session->rtp_socket < 0 ||
      !aula_rtp_payload_is_sendable(session, packet->header.payload_type))
    return AULA_STATUS_PERMISSION_DENIED;
  status = aula_rtp_serialize(packet, &output);
  if (status != AULA_STATUS_OK) return status;
  if (session->srtp_enabled != 0) {
    (void)memcpy(protected_output.data, output.data, output.length);
    protected_output.length = output.length;
    status = aula_rtp_srtp_protect_rtp(session, &protected_output);
    if (status != AULA_STATUS_OK) return status;
    output = protected_output;
  }
  status = aula_rtp_send_to_address(session->rtp_socket, session->remote_target.rtp_address,
                                   session->remote_target.rtp_address_length,
                                   session->remote_target.rtp_port,
                                   (aula_bytes){output.data, output.length});
  if (status == AULA_STATUS_OK && session->retransmit != NULL) {
    uint64_t now_ns = 0U;
    if (aula_platform_monotonic_now(&now_ns) == AULA_STATUS_OK)
      aula_rtp_retransmit_store(session, packet,
          (aula_bytes){output.data, output.length}, now_ns);
  }
  return status;
}

aula_status aula_rtp_session_send_rtcp(aula_rtp_session *session, aula_bytes packet) {
  int socket_fd;
  uint8_t wire[AULA_SIPD_MAX_RTCP_PACKET_BYTES + AULA_RTP_SRTP_RTCP_OVERHEAD_BYTES];
  aula_mutable_bytes protected_output = {wire, sizeof(wire), 0U};
  aula_status status;
  if (session == NULL || !session->remote_target_set || packet.data == NULL || packet.length == 0U ||
      packet.length > AULA_SIPD_MAX_RTCP_PACKET_BYTES) return AULA_STATUS_INVALID_ARGUMENT;
  socket_fd = session->ports.rtcp_mux ? session->rtp_socket : session->rtcp_socket;
  if (socket_fd < 0) return AULA_STATUS_STATE_ERROR;
  if (session->srtp_enabled != 0) {
    (void)memcpy(protected_output.data, packet.data, packet.length);
    protected_output.length = packet.length;
    status = aula_rtp_srtp_protect_rtcp(session, &protected_output);
    if (status != AULA_STATUS_OK) return status;
    return aula_rtp_send_to_address(socket_fd, session->remote_target.rtcp_address,
                                     session->remote_target.rtcp_address_length,
                                     session->remote_target.rtcp_port,
                                     (aula_bytes){protected_output.data, protected_output.length});
  }
  return aula_rtp_send_to_address(socket_fd, session->remote_target.rtcp_address,
                                   session->remote_target.rtcp_address_length,
                                   session->remote_target.rtcp_port, packet);
}

void aula_rtp_session_destroy(aula_rtp_session *session) {
  if (session == NULL) return;
  aula_rtp_srtp_destroy(session);
  aula_rtp_close_sockets(session);
  aula_rtp_retransmit_destroy(session);
  (void)memset(session, 0, sizeof(*session));
  free(session);
}
