#include "session_internal.h"
#include "payload_private.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define AULA_RTP_FIXED_HEADER_BYTES 12U
#define AULA_RTP_SEQUENCE_WINDOW_BITS 64U

uint16_t aula_rtp_read_u16(const uint8_t *data) {
  return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

uint32_t aula_rtp_read_u32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

int aula_rtp_address_is_valid(const uint8_t *address, uint8_t address_length) {
  size_t index;
  int nonzero = 0;
  if (address == NULL || (address_length != 4U && address_length != 16U)) return 0;
  for (index = 0U; index < address_length; ++index) {
    if (address[index] != 0U) nonzero = 1;
  }
  return nonzero;
}

int aula_rtp_source_is_valid(const aula_rtp_source *source) {
  return source != NULL && source->port != 0U &&
         aula_rtp_address_is_valid(source->address, source->address_length);
}

int aula_rtp_remote_target_is_valid(const aula_rtp_remote_target *target) {
  if (target == NULL ||
      !aula_rtp_address_is_valid(target->rtp_address, target->rtp_address_length) ||
      !aula_rtp_address_is_valid(target->rtcp_address, target->rtcp_address_length) ||
      target->rtp_port == 0U || target->rtcp_port == 0U) {
    return 0;
  }
  if (target->rtcp_mux == 0) {
    return 1;
  }
  return target->rtcp_port == target->rtp_port &&
         target->rtcp_address_length == target->rtp_address_length &&
         memcmp(target->rtcp_address, target->rtp_address,
                target->rtp_address_length) == 0;
}

int aula_rtp_payload_is_accepted(const aula_rtp_session *session,
                                         uint8_t payload_type) {
  size_t index;
  for (index = 0U; index < session->accepted_payload_count; ++index) {
    if (session->accepted_payloads[index] == payload_type) return 1;
  }
  return 0;
}

int aula_rtp_payload_is_sendable(const aula_rtp_session *session,
                                  uint8_t payload_type) {
  size_t index;
  for (index = 0U; index < session->outbound_payload_count; ++index) {
    if (session->outbound_payloads[index] == payload_type) return 1;
  }
  return 0;
}

int aula_rtp_mux_payloads_are_safe(const aula_rtp_session *session) {
  size_t index;
  for (index = 0U; index < session->accepted_payload_count; ++index) {
    if (session->accepted_payloads[index] >= 64U && session->accepted_payloads[index] <= 95U) {
      return 0;
    }
  }
  return 1;
}

static int aula_rtp_peer_policy_is_valid(const aula_rtp_peer_policy *peer) {
  if (peer->bind_address_length != 0U &&
      !aula_rtp_address_is_valid(peer->bind_address, peer->bind_address_length)) return 0;
  if ((peer->expected_peer == NULL && peer->expected_peer_authenticated != 0) ||
      (peer->expected_peer != NULL && !aula_rtp_source_is_valid(peer->expected_peer))) return 0;
  if ((peer->negotiated_remote == NULL && peer->negotiated_remote_authenticated != 0) ||
      (peer->negotiated_remote != NULL &&
       !aula_rtp_remote_target_is_valid(peer->negotiated_remote))) return 0;
  return 1;
}

static int aula_rtp_payload_policy_is_valid(const aula_rtp_payload_set *payloads) {
  size_t index;
  if (payloads->payload_types == NULL || payloads->payload_type_count == 0U ||
      payloads->payload_type_count > 128U) return 0;
  for (index = 0U; index < payloads->payload_type_count; ++index) {
    if (payloads->payload_types[index] > 127U) return 0;
  }
  return 1;
}

aula_status aula_rtp_session_configure_directional_payloads(
    aula_rtp_session *session, const aula_rtp_payload_set *inbound,
    const aula_rtp_payload_set *outbound) {
  if (session == NULL || !aula_rtp_payload_policy_is_valid(inbound) ||
      !aula_rtp_payload_policy_is_valid(outbound))
    return AULA_STATUS_INVALID_ARGUMENT;
  (void)memcpy(session->accepted_payloads, inbound->payload_types,
               inbound->payload_type_count);
  session->accepted_payload_count = inbound->payload_type_count;
  (void)memcpy(session->outbound_payloads, outbound->payload_types,
               outbound->payload_type_count);
  session->outbound_payload_count = outbound->payload_type_count;
  return AULA_STATUS_OK;
}

int aula_rtp_policy_is_valid(const aula_rtp_session_policy *policy,
                              const aula_rtp_identity *identity) {
  if (policy == NULL || identity == NULL ||
      policy->maximum_packet_bytes < AULA_RTP_FIXED_HEADER_BYTES ||
      policy->maximum_packet_bytes > AULA_SIPD_MAX_RTP_PACKET_BYTES) return 0;
  return aula_rtp_payload_policy_is_valid(&policy->accepted_payloads) &&
         aula_rtp_peer_policy_is_valid(&policy->peer);
}

static aula_status aula_rtp_set_socket_flags(int socket_fd) {
  int flags = fcntl(socket_fd, F_GETFL, 0);
  int descriptor_flags;
  if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) return AULA_STATUS_IO_ERROR;
  descriptor_flags = fcntl(socket_fd, F_GETFD, 0);
  if (descriptor_flags < 0 || fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    return AULA_STATUS_IO_ERROR;
  }
  return AULA_STATUS_OK;
}

static aula_status aula_rtp_make_sockaddr(const uint8_t *address, uint8_t address_length,
                                            uint16_t port, struct sockaddr_storage *out_address,
                                            socklen_t *out_length) {
  if (!aula_rtp_address_is_valid(address, address_length) || port == 0U ||
      out_address == NULL || out_length == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_address, 0, sizeof(*out_address));
  if (address_length == 4U) {
    struct sockaddr_in *address_v4 = (struct sockaddr_in *)out_address;
    address_v4->sin_family = AF_INET;
    address_v4->sin_port = (in_port_t)htons(port);
    (void)memcpy(&address_v4->sin_addr, address, 4U);
    *out_length = (socklen_t)sizeof(*address_v4);
  } else {
    struct sockaddr_in6 *address_v6 = (struct sockaddr_in6 *)out_address;
    address_v6->sin6_family = AF_INET6;
    address_v6->sin6_port = (in_port_t)htons(port);
    (void)memcpy(&address_v6->sin6_addr, address, 16U);
    *out_length = (socklen_t)sizeof(*address_v6);
  }
  return AULA_STATUS_OK;
}

static aula_status aula_rtp_bind_udp(const uint8_t *address, uint8_t address_length,
                                       uint16_t port, int *out_socket) {
  struct sockaddr_storage socket_address;
  socklen_t socket_address_length;
  int socket_fd;
  int family;
  aula_status status;
  if (out_socket == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_rtp_make_sockaddr(address, address_length, port, &socket_address,
                                   &socket_address_length);
  if (status != AULA_STATUS_OK) return status;
  family = address_length == 4U ? AF_INET : AF_INET6;
  socket_fd = socket(family, SOCK_DGRAM, 0);
  if (socket_fd < 0) return AULA_STATUS_IO_ERROR;
  status = aula_rtp_set_socket_flags(socket_fd);
  if (status != AULA_STATUS_OK ||
      bind(socket_fd, (const struct sockaddr *)&socket_address, socket_address_length) != 0) {
    (void)close(socket_fd);
    return status == AULA_STATUS_OK ? AULA_STATUS_IO_ERROR : status;
  }
  *out_socket = socket_fd;
  return AULA_STATUS_OK;
}

void aula_rtp_close_sockets(aula_rtp_session *session) {
  if (session->rtp_socket >= 0) {
    (void)close(session->rtp_socket);
    session->rtp_socket = -1;
  }
  if (session->rtcp_socket >= 0) {
    (void)close(session->rtcp_socket);
    session->rtcp_socket = -1;
  }
}

size_t aula_rtp_session_get_descriptors_internal(
    const aula_rtp_session *session, int *descriptors, size_t capacity) {
  size_t count = 0U;
  if (session == NULL || descriptors == NULL) return 0U;
  if (session->rtp_socket >= 0 && count < capacity)
    descriptors[count++] = session->rtp_socket;
  if (session->rtcp_socket >= 0 && session->rtcp_socket != session->rtp_socket &&
      count < capacity)
    descriptors[count++] = session->rtcp_socket;
  return count;
}

aula_status aula_rtp_bind_pair(aula_rtp_session *session,
                                        const uint8_t *address, uint8_t address_length,
                                        uint16_t rtp_port, int rtcp_mux) {
  aula_status status;
  if (rtcp_mux && !aula_rtp_mux_payloads_are_safe(session)) return AULA_STATUS_CONFIGURATION_ERROR;
  aula_rtp_close_sockets(session);
  status = aula_rtp_bind_udp(address, address_length, rtp_port, &session->rtp_socket);
  if (status != AULA_STATUS_OK) return status;
  if (!rtcp_mux) {
    if (rtp_port == UINT16_MAX) {
      aula_rtp_close_sockets(session);
      return AULA_STATUS_INVALID_ARGUMENT;
    }
    status = aula_rtp_bind_udp(address, address_length, (uint16_t)(rtp_port + 1U),
                                &session->rtcp_socket);
    if (status != AULA_STATUS_OK) {
      aula_rtp_close_sockets(session);
      return status;
    }
  }
  session->ports.rtp_port = rtp_port;
  session->ports.rtcp_port = rtcp_mux ? rtp_port : (uint16_t)(rtp_port + 1U);
  session->ports.rtcp_mux = rtcp_mux ? 1 : 0;
  return AULA_STATUS_OK;
}

aula_status aula_rtp_session_init(const aula_rtp_identity *identity,
                                           const aula_rtp_session_policy *policy,
                                           aula_rtp_session **out_session) {
  aula_rtp_session *session;
  if (identity == NULL || out_session == NULL || identity->payload_type > 127U ||
      !aula_rtp_policy_is_valid(policy, identity)) return AULA_STATUS_INVALID_ARGUMENT;
  *out_session = NULL;
  session = (aula_rtp_session *)calloc(1U, sizeof(*session));
  if (session == NULL) return AULA_STATUS_INTERNAL_ERROR;
  session->rtp_socket = -1;
  session->rtcp_socket = -1;
  session->identity = *identity;
  (void)memcpy(session->accepted_payloads, policy->accepted_payloads.payload_types,
               policy->accepted_payloads.payload_type_count);
  session->accepted_payload_count = policy->accepted_payloads.payload_type_count;
  (void)memcpy(session->outbound_payloads, policy->accepted_payloads.payload_types,
               policy->accepted_payloads.payload_type_count);
  session->outbound_payload_count = policy->accepted_payloads.payload_type_count;
  session->maximum_packet_bytes = policy->maximum_packet_bytes;
  session->symmetric_rtp_enabled = policy->peer.symmetric_rtp_enabled != 0;
  if (policy->peer.expected_peer_authenticated != 0) {
    session->source = *policy->peer.expected_peer;
    session->source_locked = 1;
    session->source_ssrc_learned = 1;
  }
  if (policy->peer.negotiated_remote_authenticated != 0) {
    session->remote_target = *policy->peer.negotiated_remote;
    session->remote_target_set = 1;
    (void)memcpy(session->source.address, session->remote_target.rtp_address,
                 session->remote_target.rtp_address_length);
    session->source.address_length = session->remote_target.rtp_address_length;
    session->source.port = session->remote_target.rtp_port;
    session->source_locked = 1;
    session->source_ssrc_learned = 0;
    (void)memcpy(session->rtcp_source.address, session->remote_target.rtcp_address,
                 session->remote_target.rtcp_address_length);
    session->rtcp_source.address_length = session->remote_target.rtcp_address_length;
    session->rtcp_source.port = session->remote_target.rtcp_port;
    session->rtcp_source_locked = 1;
  }
  *out_session = session;
  return AULA_STATUS_OK;
}

void aula_rtp_account_sequence(aula_rtp_session *session, uint16_t sequence_number) {
  uint16_t forward_distance;
  session->counters.packets++;
  if (!session->sequence_initialized) {
    session->highest_sequence = sequence_number;
    session->sequence_window = UINT64_C(1);
    session->sequence_initialized = 1;
    return;
  }
  forward_distance = (uint16_t)(sequence_number - session->highest_sequence);
  if (forward_distance != 0U && forward_distance < UINT16_C(0x8000)) {
    if (forward_distance > 1U) session->counters.lost += (uint64_t)(forward_distance - 1U);
    if (forward_distance >= AULA_RTP_SEQUENCE_WINDOW_BITS) session->sequence_window = UINT64_C(1);
    else session->sequence_window = (session->sequence_window << forward_distance) | UINT64_C(1);
    session->highest_sequence = sequence_number;
    return;
  }
  if (forward_distance == 0U) {
    session->counters.duplicates++;
    return;
  }
  {
    uint16_t distance = (uint16_t)(session->highest_sequence - sequence_number);
    if (distance < AULA_RTP_SEQUENCE_WINDOW_BITS &&
        (session->sequence_window & (UINT64_C(1) << distance)) != 0U) {
      session->counters.duplicates++;
      return;
    }
    if (distance < AULA_RTP_SEQUENCE_WINDOW_BITS) session->sequence_window |= UINT64_C(1) << distance;
    if (session->counters.lost != 0U) session->counters.lost--;
    session->counters.reordered++;
  }
}

aula_status aula_rtp_source_from_sockaddr(const struct sockaddr_storage *address,
                                                    socklen_t address_length,
                                                    aula_rtp_source *out_source) {
  if (address == NULL || out_source == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_source, 0, sizeof(*out_source));
  if (address->ss_family == AF_INET && address_length >= (socklen_t)sizeof(struct sockaddr_in)) {
    const struct sockaddr_in *address_v4 = (const struct sockaddr_in *)address;
    (void)memcpy(out_source->address, &address_v4->sin_addr, 4U);
    out_source->address_length = 4U;
    out_source->port = (uint16_t)ntohs(address_v4->sin_port);
  } else if (address->ss_family == AF_INET6 && address_length >= (socklen_t)sizeof(struct sockaddr_in6)) {
    const struct sockaddr_in6 *address_v6 = (const struct sockaddr_in6 *)address;
    (void)memcpy(out_source->address, &address_v6->sin6_addr, 16U);
    out_source->address_length = 16U;
    out_source->port = (uint16_t)ntohs(address_v6->sin6_port);
  } else return AULA_STATUS_INVALID_DATA;
  return aula_rtp_source_is_valid(out_source) ? AULA_STATUS_OK : AULA_STATUS_INVALID_DATA;
}

aula_status aula_rtp_send_to_address(int socket_fd, const uint8_t *target_address,
                                              uint8_t target_address_length, uint16_t target_port,
                                              aula_bytes packet) {
  struct sockaddr_storage address;
  socklen_t address_length;
  ssize_t written;
  aula_status status;
  if (socket_fd < 0 || !aula_rtp_address_is_valid(target_address, target_address_length) ||
      target_port == 0U ||
      (packet.length != 0U && packet.data == NULL)) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_rtp_make_sockaddr(target_address, target_address_length, target_port,
                                   &address, &address_length);
  if (status != AULA_STATUS_OK) return status;
  written = sendto(socket_fd, packet.data, packet.length, 0,
                   (const struct sockaddr *)&address, address_length);
  if (written < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
      AULA_STATUS_AGAIN : AULA_STATUS_IO_ERROR;
  return (size_t)written == packet.length ? AULA_STATUS_OK : AULA_STATUS_IO_ERROR;
}

aula_status aula_rtp_receive_datagram(int socket_fd, aula_mutable_bytes *packet_buffer,
                                                struct sockaddr_storage *out_address,
                                                socklen_t *out_address_length) {
  struct iovec vector;
  struct msghdr message;
  ssize_t received;
  if (socket_fd < 0 || packet_buffer == NULL || packet_buffer->data == NULL ||
      packet_buffer->capacity == 0U || out_address == NULL || out_address_length == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(&message, 0, sizeof(message));
  vector.iov_base = packet_buffer->data;
  vector.iov_len = packet_buffer->capacity;
  message.msg_name = out_address;
  message.msg_namelen = (socklen_t)sizeof(*out_address);
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  received = recvmsg(socket_fd, &message, 0);
  if (received < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ?
        AULA_STATUS_AGAIN : AULA_STATUS_IO_ERROR;
  }
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  packet_buffer->length = (size_t)received;
  *out_address_length = message.msg_namelen;
  return AULA_STATUS_OK;
}
