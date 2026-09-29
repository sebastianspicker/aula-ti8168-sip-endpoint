#define _POSIX_C_SOURCE 200809L

#include "gateway_internal.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
extern int getpeereid(int descriptor, uid_t *uid, gid_t *gid);
#endif

int aula_gateway_frame_validate(const uint8_t *frame, size_t frame_length,
                                 uint8_t expected_opcode, uint32_t expected_request_id) {
  uint16_t flags;
  uint32_t request_id;
  uint32_t length;
  if (frame == NULL || frame_length < AULA_CONTROL_HEADER_BYTES ||
      memcmp(frame, AULA_CONTROL_MAGIC, 4U) != 0 || frame[4] != AULA_CONTROL_PROTOCOL_VERSION ||
      frame[5] != expected_opcode || expected_opcode < 1U || expected_opcode > AULA_CONTROL_OPCODE_METRICS)
    return 0;
  (void)memcpy(&flags, frame + 6U, sizeof(flags));
  (void)memcpy(&request_id, frame + 8U, sizeof(request_id));
  (void)memcpy(&length, frame + 12U, sizeof(length));
  flags = ntohs(flags);
  request_id = ntohl(request_id);
  length = ntohl(length);
  return (flags & AULA_CONTROL_FRAME_RESPONSE) != 0U &&
      (flags & ~(AULA_CONTROL_FRAME_RESPONSE | AULA_CONTROL_FRAME_ERROR | AULA_CONTROL_FRAME_EVENT)) == 0U &&
      request_id == expected_request_id && length <= GATEWAY_LSZ1_MAX_PAYLOAD &&
      frame_length == AULA_CONTROL_HEADER_BYTES + (size_t)length;
}

int aula_gateway_peer_uid_matches(int descriptor, uint32_t expected_uid) {
  if (descriptor < 0) return 0;
#if defined(__linux__)
  {
    struct ucred credentials;
    socklen_t length = (socklen_t)sizeof(credentials);
    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
        length == sizeof(credentials) && credentials.uid >= 0 && (uint32_t)credentials.uid == expected_uid;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__)
  {
    uid_t uid;
    gid_t gid;
    return getpeereid(descriptor, &uid, &gid) == 0 && (uint32_t)uid == expected_uid;
  }
#else
  (void)expected_uid;
  return 0;
#endif
}

static int split_control_socket_path(const aula_gateway_config *config, char parent_path[512],
                                     const char **socket_name) {
  const char *slash;
  size_t parent_length;
  if (config == NULL || config->control_socket_path == NULL || config->control_socket_path[0] != '/') return 0;
  slash = strrchr(config->control_socket_path, '/');
  if (slash == NULL || slash == config->control_socket_path || slash[1] == '\0' ||
      strlen(slash + 1U) > NAME_MAX) return 0;
  parent_length = (size_t)(slash - config->control_socket_path);
  if (parent_length >= 512U) return 0;
  (void)memcpy(parent_path, config->control_socket_path, parent_length);
  parent_path[parent_length] = '\0';
  *socket_name = slash + 1U;
  return 1;
}

static int open_verified_control_directory(const aula_gateway_config *config, const char *parent_path) {
  char parent_name[NAME_MAX + 1U];
  struct stat expected;
  struct stat actual;
  int grandparent;
  int parent;
  if (!gateway_open_parent_directory(parent_path, &grandparent, parent_name)) return -1;
  if (fstatat(grandparent, parent_name, &expected, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(expected.st_mode) || expected.st_uid != (uid_t)config->expected_sipd_uid ||
      (expected.st_mode & 0022U) != 0U) {
    (void)close(grandparent);
    return -1;
  }
  parent = openat(grandparent, parent_name, O_RDONLY | O_DIRECTORY | AULA_O_NOFOLLOW);
  (void)close(grandparent);
  if (parent < 0 || fstat(parent, &actual) != 0 ||
      actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino) {
    if (parent >= 0) (void)close(parent);
    return -1;
  }
  return parent;
}

static int open_control_parent(const aula_gateway_config *config, int *parent_out, char name[NAME_MAX + 1U]) {
  char parent_path[512];
  const char *socket_name;
  int parent;
  if (parent_out == NULL || name == NULL || !split_control_socket_path(config, parent_path, &socket_name))
    return 0;
  parent = open_verified_control_directory(config, parent_path);
  if (parent < 0) return 0;
  (void)snprintf(name, NAME_MAX + 1U, "%s", socket_name);
  *parent_out = parent;
  return 1;
}

int aula_gateway_control_socket_is_safe(const aula_gateway_config *config) {
  char name[NAME_MAX + 1U];
  struct stat details;
  int parent;
  int result;
  if (!open_control_parent(config, &parent, name)) return 0;
  result = fstatat(parent, name, &details, AT_SYMLINK_NOFOLLOW) == 0 && S_ISSOCK(details.st_mode) &&
      details.st_uid == (uid_t)config->expected_sipd_uid && (details.st_mode & 0007U) == 0U;
  (void)close(parent);
  return result;
}

static int control_exchange_input_is_valid(const aula_gateway_config *config, const char *payload,
                                           char *response, size_t response_capacity) {
  return config != NULL && config->control_socket_path != NULL &&
      payload != NULL && response != NULL && response_capacity != 0U &&
      config->control_timeout_milliseconds <= AULA_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS &&
      strlen(config->control_socket_path) < sizeof(((struct sockaddr_un *)0)->sun_path) &&
      aula_gateway_control_socket_is_safe(config);
}

static void prepare_control_address(struct sockaddr_un *address, const char *socket_path) {
  (void)memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  (void)snprintf(address->sun_path, sizeof(address->sun_path), "%s", socket_path);
}

int gateway_monotonic_milliseconds(uint64_t *milliseconds) {
  struct timespec now;
  if (milliseconds == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0)
    return 0;
  *milliseconds = (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
  return 1;
}

static int control_deadline(const aula_gateway_config *config, uint64_t *deadline) {
  uint64_t now;
  unsigned int timeout;
  if (!gateway_monotonic_milliseconds(&now) || config == NULL || deadline == NULL) return 0;
  timeout = config->control_timeout_milliseconds == 0U ?
      AULA_GATEWAY_CONTROL_TIMEOUT_MILLISECONDS : config->control_timeout_milliseconds;
  *deadline = now + (uint64_t)timeout;
  return 1;
}

static int deadline_remaining_milliseconds(uint64_t deadline, int *remaining) {
  uint64_t now;
  uint64_t difference;
  if (!gateway_monotonic_milliseconds(&now) || remaining == NULL || now >= deadline) return 0;
  difference = deadline - now;
  *remaining = (int)(difference > (uint64_t)INT_MAX ? INT_MAX : difference);
  return *remaining > 0;
}

static int wait_for_control_socket(int descriptor, short events, uint64_t deadline, short *revents) {
  struct pollfd waiting;
  int remaining;
  int result;
  if (revents == NULL) return 0;
  waiting.fd = descriptor;
  waiting.events = events;
  waiting.revents = 0;
  for (;;) {
    if (!deadline_remaining_milliseconds(deadline, &remaining)) return 0;
    result = poll(&waiting, 1U, remaining);
    if (result > 0) {
      *revents = waiting.revents;
      return (waiting.revents & POLLNVAL) == 0;
    }
    if (result == 0 || errno != EINTR) return 0;
  }
}

static int set_control_socket_nonblocking(int descriptor) {
  int flags;
  do {
    flags = fcntl(descriptor, F_GETFL);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) return 0;
  do {
    flags = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
  } while (flags < 0 && errno == EINTR);
  return flags >= 0;
}

static int socket_connect_complete(int descriptor) {
  int error = 0;
  socklen_t error_length = (socklen_t)sizeof(error);
  int result;
  do {
    result = getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &error_length);
  } while (result != 0 && errno == EINTR);
  return result == 0 && error_length == sizeof(error) && error == 0;
}

static int wait_for_control_connect(int descriptor, uint64_t deadline) {
  short revents;
  return wait_for_control_socket(descriptor, POLLOUT, deadline, &revents) &&
      (revents & (POLLERR | POLLHUP)) == 0 && (revents & POLLOUT) != 0 && socket_connect_complete(descriptor);
}

static int connect_control_socket(const aula_gateway_config *config, const struct sockaddr_un *address,
                                  uint64_t deadline, int *packet_socket) {
  int descriptor;
  int connected;
#if defined(__APPLE__)
  descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
#else
  descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);
#endif
  if (descriptor < 0 || packet_socket == NULL || !set_control_socket_nonblocking(descriptor)) {
    if (descriptor >= 0) (void)close(descriptor);
    return -1;
  }
  connected = connect(descriptor, (const struct sockaddr *)address, sizeof(*address));
  if (connected != 0 && errno != EINPROGRESS && errno != EALREADY && errno != EINTR) {
    (void)close(descriptor);
    return -1;
  }
  if (connected != 0 && !wait_for_control_connect(descriptor, deadline)) {
    (void)close(descriptor);
    return -1;
  }
  if (!aula_gateway_peer_uid_matches(descriptor, config->expected_sipd_uid)) {
    (void)close(descriptor);
    return -1;
  }
#if defined(__APPLE__)
  *packet_socket = 0;
#else
  *packet_socket = 1;
#endif
  return descriptor;
}

static void build_control_request(uint8_t request[AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD],
                                  uint8_t opcode, uint32_t request_id, const char *payload,
                                  size_t payload_length) {
  uint16_t flags = 0U;
  uint32_t wire_id = htonl(request_id);
  uint32_t length = htonl((uint32_t)payload_length);
  (void)memcpy(request, AULA_CONTROL_MAGIC, 4U);
  request[4] = AULA_CONTROL_PROTOCOL_VERSION;
  request[5] = opcode;
  (void)memcpy(request + 6U, &flags, sizeof(flags));
  (void)memcpy(request + 8U, &wire_id, sizeof(wire_id));
  (void)memcpy(request + 12U, &length, sizeof(length));
  (void)memcpy(request + AULA_CONTROL_HEADER_BYTES, payload, payload_length);
}

static int send_control_request(int descriptor, const uint8_t *request, size_t request_length,
                                int packet_socket, uint64_t deadline) {
  size_t sent = 0U;
  short revents;
  ssize_t result;
  while (sent < request_length) {
    if (!wait_for_control_socket(descriptor, POLLOUT, deadline, &revents) ||
        (revents & (POLLERR | POLLHUP)) != 0 || (revents & POLLOUT) == 0) return 0;
    do {
#ifdef MSG_NOSIGNAL
      result = send(descriptor, request + sent, request_length - sent, MSG_NOSIGNAL);
#else
      result = send(descriptor, request + sent, request_length - sent, 0);
#endif
    } while (result < 0 && errno == EINTR);
    if (result > 0) {
      if (packet_socket && (size_t)result != request_length) return 0;
      sent += (size_t)result;
      if (packet_socket) return 1;
    } else if (result == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      return 0;
    }
  }
  return 1;
}

static int receive_control_bytes(int descriptor, uint8_t *destination, size_t length, uint64_t deadline) {
  size_t received = 0U;
  short revents;
  ssize_t result;
  while (received < length) {
    if (!wait_for_control_socket(descriptor, POLLIN, deadline, &revents)) return 0;
    do {
      result = recv(descriptor, destination + received, length - received, 0);
    } while (result < 0 && errno == EINTR);
    if (result > 0) received += (size_t)result;
    else if (result == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return 0;
  }
  return 1;
}

static int stream_reply_has_no_trailing_bytes(int descriptor, uint64_t deadline) {
  uint8_t extra;
  short revents;
  ssize_t result;
  for (;;) {
    if (!wait_for_control_socket(descriptor, POLLIN, deadline, &revents)) return 0;
    do {
      result = recv(descriptor, &extra, sizeof(extra), 0);
    } while (result < 0 && errno == EINTR);
    if (result == 0) return 1;
    if (result > 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return 0;
  }
}

static int receive_control_reply(int descriptor,
                                 uint8_t reply[AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD],
                                 uint8_t opcode, uint32_t request_id, int packet_socket, uint64_t deadline) {
  uint32_t payload_length;
  ssize_t received;
  struct iovec iov;
  struct msghdr message;
  short revents;
  if (packet_socket) {
    if (!wait_for_control_socket(descriptor, POLLIN, deadline, &revents)) return 0;
    (void)memset(&message, 0, sizeof(message));
    iov.iov_base = reply;
    iov.iov_len = AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD;
    message.msg_iov = &iov;
    message.msg_iovlen = 1U;
    do {
      received = recvmsg(descriptor, &message, 0);
    } while (received < 0 && errno == EINTR);
    return received > 0 && (message.msg_flags & MSG_TRUNC) == 0 &&
        aula_gateway_frame_validate(reply, (size_t)received, opcode, request_id);
  }
  if (!receive_control_bytes(descriptor, reply, AULA_CONTROL_HEADER_BYTES, deadline)) return 0;
  (void)memcpy(&payload_length, reply + 12U, sizeof(payload_length));
  payload_length = ntohl(payload_length);
  if (payload_length > GATEWAY_LSZ1_MAX_PAYLOAD ||
      !receive_control_bytes(descriptor, reply + AULA_CONTROL_HEADER_BYTES, (size_t)payload_length, deadline) ||
      !stream_reply_has_no_trailing_bytes(descriptor, deadline)) return 0;
  return aula_gateway_frame_validate(reply, AULA_CONTROL_HEADER_BYTES + (size_t)payload_length,
                                      opcode, request_id);
}

static int copy_control_reply(const uint8_t reply[AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD],
                              uint8_t opcode, char *response, size_t response_capacity) {
  uint16_t flags;
  uint32_t wire_id;
  uint32_t length;
  (void)memcpy(&flags, reply + 6U, sizeof(flags));
  (void)memcpy(&wire_id, reply + 8U, sizeof(wire_id));
  (void)memcpy(&length, reply + 12U, sizeof(length));
  flags = ntohs(flags);
  wire_id = ntohl(wire_id);
  length = ntohl(length);
  (void)wire_id;
  if (length + 1U > response_capacity) return 0;
  (void)memcpy(response, reply + AULA_CONTROL_HEADER_BYTES, length);
  response[length] = '\0';
  /* Keep a bounded daemon error available to the settings route only, so a
   * stale revision remains distinguishable from a transport failure. */
  if ((flags & AULA_CONTROL_FRAME_ERROR) != 0U) return 2;
  return aula_gateway_backend_response_normalize(opcode, response, response, response_capacity);
}

int aula_gateway_control_exchange(void *context, uint8_t opcode, uint32_t request_id,
                                   const char *payload, char *response, size_t response_capacity) {
  const aula_gateway_config *config = (const aula_gateway_config *)context;
  struct sockaddr_un address;
  uint8_t request[AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD];
  uint8_t reply[AULA_CONTROL_HEADER_BYTES + GATEWAY_LSZ1_MAX_PAYLOAD];
  size_t payload_length;
  int descriptor;
  int packet_socket;
  uint64_t deadline;
  int result = 0;
  (void)memset(request, 0, sizeof(request));
  (void)memset(reply, 0, sizeof(reply));
  if (!control_exchange_input_is_valid(config, payload, response, response_capacity)) goto cleanup;
  payload_length = strlen(payload);
  if (payload_length > GATEWAY_LSZ1_MAX_PAYLOAD) goto cleanup;
  prepare_control_address(&address, config->control_socket_path);
  if (!control_deadline(config, &deadline)) goto cleanup;
  descriptor = connect_control_socket(config, &address, deadline, &packet_socket);
  if (descriptor < 0) goto cleanup;
  build_control_request(request, opcode, request_id, payload, payload_length);
  if (!send_control_request(descriptor, request, AULA_CONTROL_HEADER_BYTES + payload_length,
                            packet_socket, deadline) ||
      !receive_control_reply(descriptor, reply, opcode, request_id, packet_socket, deadline)) {
    (void)close(descriptor);
    goto cleanup;
  }
  (void)close(descriptor);
  result = copy_control_reply(reply, opcode, response, response_capacity);
cleanup:
  OPENSSL_cleanse(request, sizeof(request));
  OPENSSL_cleanse(reply, sizeof(reply));
  return result;
}

void aula_gateway_redact(char *destination, size_t capacity, const char *source) {
  const char *sensitive[] = {"password", "token", "cookie", "authorization", "secret", "sip:"};
  size_t index, offset, match;
  if (destination == NULL || capacity == 0U) return;
  if (source == NULL) {
    destination[0] = '\0';
    return;
  }
  for (index = 0U; index < sizeof(sensitive) / sizeof(sensitive[0]); ++index) {
    for (offset = 0U; source[offset] != '\0'; ++offset) {
      for (match = 0U; sensitive[index][match] != '\0' && source[offset + match] != '\0' &&
           tolower((unsigned char)source[offset + match]) == sensitive[index][match]; ++match) {}
      if (sensitive[index][match] == '\0') {
        (void)snprintf(destination, capacity, "[redacted]");
        return;
      }
    }
  }
  (void)snprintf(destination, capacity, "%.*s", (int)(capacity - 1U), source);
}
