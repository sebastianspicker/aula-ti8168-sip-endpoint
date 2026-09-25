#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "control_private.h"
#include "ls200_sipd/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void control_secure_zero(void *value, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)value;
  while (length-- > 0U) *cursor++ = 0U;
}

static uint64_t control_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static void control_server_release(ls200_control_server *server) {
  if (server == NULL) return;
  control_secure_zero(server, sizeof(*server));
  free(server);
}

static int control_path_is_safe(const char *path) {
  const unsigned char *cursor;
  size_t length;
  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 0;
  length = strlen(path);
  if (strstr(path, "/../") != NULL || strcmp(path + length - (length >= 3U ? 3U : 0U), "/..") == 0) return 0;
  for (cursor = (const unsigned char *)path; *cursor != '\0'; ++cursor) if (*cursor < 0x20U || *cursor == 0x7fU) return 0;
  return 1;
}

int ls200_control_set_nonblocking(int descriptor) {
  int flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

static ls200_status validate_parent_directory(const char *path) {
  char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char *slash;
  struct stat details;
  (void)snprintf(parent, sizeof(parent), "%s", path);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent || slash[1] == '\0') return LS200_STATUS_CONFIGURATION_ERROR;
  *slash = '\0';
  if (lstat(parent, &details) != 0 || !S_ISDIR(details.st_mode) ||
      details.st_uid != geteuid() || (details.st_mode & 0022U) != 0U) {
    return LS200_STATUS_PERMISSION_DENIED;
  }
  return LS200_STATUS_OK;
}

static ls200_status remove_stale_socket(const char *path) {
  struct stat details;
  if (lstat(path, &details) != 0) return errno == ENOENT ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
  if (!S_ISSOCK(details.st_mode) || details.st_uid != geteuid() || details.st_nlink != 1) return LS200_STATUS_SECURITY_ERROR;
  return unlink(path) == 0 ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
}

static ls200_status bind_server_socket(ls200_control_server *server) {
  struct sockaddr_un address;
  struct stat created;
  (void)memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", server->config.unix_socket_path);
  if (bind(server->descriptor, (const struct sockaddr *)&address, (socklen_t)sizeof(address)) != 0 || chmod(server->config.unix_socket_path, (mode_t)server->config.socket_mode) != 0 || lstat(server->config.unix_socket_path, &created) != 0 || !S_ISSOCK(created.st_mode) || created.st_uid != geteuid() || (created.st_mode & 0777U) != (mode_t)server->config.socket_mode || listen(server->descriptor, (int)server->config.maximum_clients) != 0) return LS200_STATUS_IO_ERROR;
  return LS200_STATUS_OK;
}

static int control_config_is_valid(const ls200_control_config *config) {
  return config != NULL &&
      control_path_is_safe(config->unix_socket_path) &&
      config->maximum_request_bytes >= 8U &&
      config->maximum_request_bytes <= LS200_SIPD_MAX_CONTROL_MESSAGE_BYTES &&
      config->maximum_clients != 0U &&
      config->maximum_clients <= LS200_CONTROL_CLIENT_LIMIT &&
      (config->socket_mode == 0600U || config->socket_mode == 0660U);
}

static void control_server_initialize_clients(ls200_control_server *server) {
  size_t index;
  server->descriptor = -1;
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index)
    server->clients[index].descriptor = -1;
}

ls200_status ls200_control_server_create(const ls200_control_config *config, ls200_control_server **out_server) {
  ls200_control_server *server;
  ls200_status status;
  if (out_server == NULL || *out_server != NULL ||
      !control_config_is_valid(config)) return LS200_STATUS_INVALID_ARGUMENT;
  status = validate_parent_directory(config->unix_socket_path);
  if (status != LS200_STATUS_OK) return status;
  status = remove_stale_socket(config->unix_socket_path);
  if (status != LS200_STATUS_OK) return status;
  server = (ls200_control_server *)calloc(1U, sizeof(*server));
  if (server == NULL) return LS200_STATUS_INTERNAL_ERROR;
  control_server_initialize_clients(server);
  {
    ls200_mutable_bytes key = {
      server->completed_cache_key, sizeof(server->completed_cache_key), 0U
    };
    status = ls200_platform_random_bytes(&key);
    if (status != LS200_STATUS_OK || key.length != key.capacity) {
      control_server_release(server);
      return status == LS200_STATUS_OK ? LS200_STATUS_INTERNAL_ERROR : status;
    }
  }
#if defined(__APPLE__)
  /* Darwin has no AF_UNIX SOCK_SEQPACKET. This host-only compatibility lane
   * still exercises LSZ1 framing; the Linux/LS200 build always uses seqpacket. */
  server->descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
#else
  server->descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);
#endif
  if (server->descriptor < 0 || !ls200_control_set_nonblocking(server->descriptor)) { if (server->descriptor >= 0) (void)close(server->descriptor); control_server_release(server); return LS200_STATUS_IO_ERROR; }
  server->config = *config;
  status = bind_server_socket(server);
  if (status != LS200_STATUS_OK) { (void)close(server->descriptor); (void)unlink(config->unix_socket_path); control_server_release(server); return status; }
  server->status.call_state = LS200_CALL_IDLE;
  (void)snprintf(server->path, sizeof(server->path), "%s", config->unix_socket_path);
  *out_server = server;
  return LS200_STATUS_OK;
}

ls200_status ls200_control_server_publish_status(ls200_control_server *server, const ls200_endpoint_status *status) {
  if (server == NULL || status == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  server->status = *status;
  return LS200_STATUS_OK;
}

static int deadline_timeout_ms(ls200_deadline deadline) {
  uint64_t now;
  uint64_t remaining_ns;
  uint64_t timeout_ms;
  if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) return -1;
  if (now >= deadline.monotonic_ns) return 0;
  remaining_ns = deadline.monotonic_ns - now;
  timeout_ms = remaining_ns / UINT64_C(1000000);
  if (remaining_ns % UINT64_C(1000000) != 0U) ++timeout_ms;
  return timeout_ms > (uint64_t)INT_MAX ? INT_MAX : (int)timeout_ms;
}

static void control_client_close(ls200_control_server *server,
                                 ls200_control_client *client) {
  if (client->descriptor >= 0) (void)close(client->descriptor);
  control_secure_zero(client, sizeof(*client));
  client->descriptor = -1;
  if (server->client_count != 0U) --server->client_count;
}

static int control_expire_clients(ls200_control_server *server, uint64_t now) {
  size_t index;
  int expired = 0;
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index) {
    ls200_control_client *client = &server->clients[index];
    if (client->descriptor >= 0 && now >= client->deadline_ns) {
      control_client_close(server, client);
      expired = 1;
    }
  }
  return expired;
}

static ls200_deadline control_effective_deadline(
    const ls200_control_server *server, ls200_deadline requested) {
  size_t index;
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index) {
    const ls200_control_client *client = &server->clients[index];
    if (client->descriptor >= 0 && client->deadline_ns < requested.monotonic_ns)
      requested.monotonic_ns = client->deadline_ns;
  }
  return requested;
}

static ls200_control_client *control_available_client(ls200_control_server *server) {
  size_t index;
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index) {
    if (server->clients[index].descriptor < 0) return &server->clients[index];
  }
  return NULL;
}

static int control_client_socket_ready(int descriptor) {
#if defined(SO_NOSIGPIPE)
  int enabled = 1;
  if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 (socklen_t)sizeof(enabled)) != 0) return 0;
#endif
  return ls200_control_set_nonblocking(descriptor);
}

static ls200_status control_accept_client(ls200_control_server *server,
                                          uint64_t now) {
  ls200_control_client *slot;
  int descriptor;
  if (server->client_count >= server->config.maximum_clients) {
    return LS200_STATUS_AGAIN;
  }
  descriptor = accept(server->descriptor, NULL, NULL);
  if (descriptor < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK
      ? LS200_STATUS_AGAIN : LS200_STATUS_IO_ERROR;
  if (!ls200_control_peer_is_authorized(server, descriptor)) {
    (void)close(descriptor);
    return LS200_STATUS_PERMISSION_DENIED;
  }
  slot = control_available_client(server);
  if (slot == NULL || !control_client_socket_ready(descriptor)) {
    (void)close(descriptor);
    return slot == NULL ? LS200_STATUS_LIMIT_EXCEEDED : LS200_STATUS_IO_ERROR;
  }
  slot->descriptor = descriptor;
  slot->deadline_ns = control_saturating_add(
      now, LS200_CONTROL_CONNECTION_TIMEOUT_NS);
  ++server->client_count;
  return LS200_STATUS_OK;
}

static ls200_status control_flush_client(ls200_control_client *client) {
  ssize_t sent;
  sent = send(client->descriptor, client->response + client->response_offset,
              client->response_length - client->response_offset, MSG_NOSIGNAL);
  if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
    return LS200_STATUS_AGAIN;
  if (sent <= 0) return LS200_STATUS_IO_ERROR;
  client->response_offset += (size_t)sent;
  return client->response_offset == client->response_length
      ? LS200_STATUS_END : LS200_STATUS_AGAIN;
}

static ls200_status control_dispatch_ready(ls200_control_server *server,
                                           struct pollfd waits[]) {
  size_t offset;
  for (offset = 0U; offset < LS200_CONTROL_CLIENT_LIMIT; ++offset) {
    size_t index = (server->next_client + offset) % LS200_CONTROL_CLIENT_LIMIT;
    ls200_control_client *client = &server->clients[index];
    short events = waits[index + 1U].revents;
    ls200_status status;
    if (client->descriptor < 0 || events == 0) continue;
    server->next_client = (index + 1U) % LS200_CONTROL_CLIENT_LIMIT;
    if ((events & (POLLERR | POLLNVAL)) != 0) {
      control_client_close(server, client);
      return LS200_STATUS_OK;
    }
    if (client->response_length != 0U && (events & POLLOUT) != 0) {
      status = control_flush_client(client);
    } else if (client->response_length == 0U && (events & POLLIN) != 0) {
      status = ls200_control_handle_client(server, client);
    } else if ((events & POLLHUP) != 0) {
      status = LS200_STATUS_END;
    } else {
      return LS200_STATUS_AGAIN;
    }
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN)
      control_client_close(server, client);
    return LS200_STATUS_OK;
  }
  return LS200_STATUS_AGAIN;
}

static void control_prepare_waits(ls200_control_server *server,
                                  struct pollfd waits[],
                                  const int *readiness_descriptors,
                                  size_t readiness_count) {
  size_t index;
  waits[0] = (struct pollfd){server->descriptor,
      server->client_count < server->config.maximum_clients ? POLLIN : 0, 0};
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index) {
    ls200_control_client *client = &server->clients[index];
    short events = client->response_length == 0U ? POLLIN : POLLOUT;
    waits[index + 1U] = (struct pollfd){client->descriptor, events, 0};
  }
  for (index = 0U; index < readiness_count; ++index) {
    waits[LS200_CONTROL_CLIENT_LIMIT + 1U + index] =
        (struct pollfd){readiness_descriptors[index], POLLIN, 0};
  }
}

static int control_poll_waits(struct pollfd waits[], size_t wait_count,
                              ls200_deadline deadline) {
  int polled;
  do {
    int timeout_ms = deadline_timeout_ms(deadline);
    if (timeout_ms < 0) return -1;
    polled = poll(waits, (nfds_t)wait_count, timeout_ms);
  } while (polled < 0 && errno == EINTR);
  return polled;
}

static ls200_status control_process_poll(ls200_control_server *server,
                                         struct pollfd waits[], int polled,
                                         uint64_t now, ls200_status result) {
  if (control_expire_clients(server, now)) result = LS200_STATUS_OK;
  if (polled == 0) return result;
  if (control_dispatch_ready(server, waits) == LS200_STATUS_OK)
    result = LS200_STATUS_OK;
  if ((waits[0].revents & POLLIN) != 0 &&
      control_accept_client(server, now) == LS200_STATUS_OK)
    result = LS200_STATUS_OK;
  return result;
}

ls200_status ls200_control_server_poll(ls200_control_server *server,
                                       ls200_deadline deadline) {
  return ls200_control_server_poll_with_readiness(server, deadline, NULL, 0U);
}

ls200_status ls200_control_server_poll_with_readiness(
    ls200_control_server *server, ls200_deadline deadline,
    const int *readiness_descriptors, size_t readiness_count) {
  struct pollfd waits[LS200_CONTROL_CLIENT_LIMIT + 1U +
                      LS200_CONTROL_READINESS_DESCRIPTOR_LIMIT];
  ls200_deadline effective;
  ls200_status result = LS200_STATUS_AGAIN;
  uint64_t now;
  int polled;
  if (server == NULL ||
      readiness_count > LS200_CONTROL_READINESS_DESCRIPTOR_LIMIT ||
      (readiness_count != 0U && readiness_descriptors == NULL))
    return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK)
    return LS200_STATUS_IO_ERROR;
  if (control_expire_clients(server, now)) result = LS200_STATUS_OK;
  effective = control_effective_deadline(server, deadline);
  control_prepare_waits(server, waits, readiness_descriptors, readiness_count);
  polled = control_poll_waits(
      waits, LS200_CONTROL_CLIENT_LIMIT + 1U + readiness_count, effective);
  if (polled < 0) return LS200_STATUS_IO_ERROR;
  if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK)
    return LS200_STATUS_IO_ERROR;
  if (polled > 0) {
    size_t index;
    for (index = 0U; index < readiness_count; ++index) {
      if (waits[LS200_CONTROL_CLIENT_LIMIT + 1U + index].revents != 0) {
        result = LS200_STATUS_OK;
        break;
      }
    }
  }
  return control_process_poll(server, waits, polled, now, result);
}

void ls200_control_server_destroy(ls200_control_server *server) {
  struct stat details;
  size_t index;
  if (server == NULL) return;
  for (index = 0U; index < LS200_CONTROL_CLIENT_LIMIT; ++index) {
    if (server->clients[index].descriptor >= 0)
      control_client_close(server, &server->clients[index]);
  }
  if (server->descriptor >= 0) (void)close(server->descriptor);
  if (lstat(server->path, &details) == 0 && S_ISSOCK(details.st_mode) && details.st_uid == geteuid() && details.st_nlink == 1) (void)unlink(server->path);
  control_server_release(server);
}
