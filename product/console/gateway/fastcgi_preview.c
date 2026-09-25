#define _POSIX_C_SOURCE 200809L
#include "fastcgi_preview.h"

#include "preview/preview_flv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LS200_PREVIEW_CONNECT_TIMEOUT_MS 2000
#define LS200_PREVIEW_LEASE_POLL_MS 1000
#define LS200_PREVIEW_ACCESS_UNIT_BYTES (2U * 1024U * 1024U)

typedef struct preview_output {
  FCGX_Request *request;
  int headers_written;
} preview_output;

static void write_gateway_response(FCGX_Request *request,
                                   const ls200_gateway_response *response) {
  (void)FCGX_FPrintF(request->out, "Status: %u\r\nContent-Type: %s\r\n",
                    response->status, response->content_type);
  if (response->set_cookie[0] != '\0') {
    (void)FCGX_FPrintF(request->out, "Set-Cookie: %s\r\n",
                      response->set_cookie);
  }
  (void)FCGX_FPrintF(request->out,
                    "Cache-Control: no-store\r\n"
                    "X-Content-Type-Options: nosniff\r\n\r\n%s",
                    response->body);
}

static void write_preview_error(FCGX_Request *request, unsigned int status,
                                const char *code, const char *message) {
  (void)FCGX_FPrintF(
      request->out,
      "Status: %u\r\nContent-Type: application/json\r\n"
      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n"
      "{\"revision\":1,\"ok\":false,\"error\":{\"code\":\"%s\","
      "\"message\":\"%s\"}}",
      status, code, message);
}

int ls200_fastcgi_is_preview_path(const char *path) {
  return path != NULL &&
         strcmp(path, "/zoom/api/v1/media/preview.flv") == 0;
}

static int reserve_preview(ls200_fastcgi_server *server,
                           const ls200_gateway_request *request,
                           ls200_gateway_preview_lease *lease,
                           ls200_gateway_response *response) {
  int authorized;
  authorized = ls200_gateway_preview_authorize(&server->gateway, request, lease,
                                               response);
  (void)pthread_mutex_lock(&server->preview_mutex);
  if (authorized && server->preview_busy) authorized = -1;
  if (authorized == 1) server->preview_busy = 1;
  (void)pthread_mutex_unlock(&server->preview_mutex);
  return authorized;
}

static void release_preview(ls200_fastcgi_server *server) {
  (void)pthread_mutex_lock(&server->preview_mutex);
  server->preview_busy = 0;
  (void)pthread_mutex_unlock(&server->preview_mutex);
}

static int lease_is_valid(ls200_fastcgi_server *server,
                          const ls200_gateway_preview_lease *lease) {
  return ls200_gateway_preview_lease_valid(&server->gateway, lease,
                                           (uint64_t)time(NULL));
}

static int wait_for_descriptor(int descriptor, short events, int timeout_ms) {
  struct pollfd poll_descriptor = {descriptor, events, 0};
  int result;
  do {
    result = poll(&poll_descriptor, 1U, timeout_ms);
  } while (result < 0 && errno == EINTR);
  return result == 1 && (poll_descriptor.revents & events) != 0 &&
         (poll_descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
}

static int connect_preview_socket(const ls200_gateway_config *config) {
  struct sockaddr_in address;
  int descriptor;
  int flags;
  int error = 0;
  socklen_t error_length = sizeof(error);
  descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) return -1;
  flags = fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    (void)close(descriptor);
    return -1;
  }
  (void)memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(config->preview_rtsp_port);
  if (inet_pton(AF_INET, config->preview_rtsp_ipv4, &address.sin_addr) != 1) {
    (void)close(descriptor);
    return -1;
  }
  if (connect(descriptor, (const struct sockaddr *)&address,
              sizeof(address)) == 0) return descriptor;
  if (errno != EINPROGRESS ||
      !wait_for_descriptor(descriptor, POLLOUT,
                           LS200_PREVIEW_CONNECT_TIMEOUT_MS) ||
      getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 ||
      error != 0) {
    (void)close(descriptor);
    return -1;
  }
  return descriptor;
}

static int send_all(int descriptor, ls200_bytes bytes) {
  size_t offset = 0U;
  while (offset < bytes.length) {
    ssize_t written;
    if (!wait_for_descriptor(descriptor, POLLOUT,
                             LS200_PREVIEW_CONNECT_TIMEOUT_MS)) return 0;
    written = send(descriptor, bytes.data + offset, bytes.length - offset,
#ifdef MSG_NOSIGNAL
                   MSG_NOSIGNAL
#else
                   0
#endif
    );
    if (written > 0) {
      offset += (size_t)written;
    } else if (written == 0 || (errno != EINTR && errno != EAGAIN &&
                                errno != EWOULDBLOCK)) {
      return 0;
    }
  }
  return 1;
}

static ls200_status write_flv(void *context, ls200_bytes bytes) {
  preview_output *output = (preview_output *)context;
  size_t offset = 0U;
  if (!output->headers_written) {
    if (FCGX_FPrintF(output->request->out,
                    "Status: 200 OK\r\nContent-Type: video/x-flv\r\n"
                    "Cache-Control: no-store\r\n"
                    "X-Content-Type-Options: nosniff\r\n\r\n") < 0) {
      return LS200_STATUS_IO_ERROR;
    }
    output->headers_written = 1;
  }
  while (offset < bytes.length) {
    size_t remaining = bytes.length - offset;
    int chunk = remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
    int written = FCGX_PutStr((const char *)bytes.data + offset, chunk,
                              output->request->out);
    if (written != chunk) return LS200_STATUS_IO_ERROR;
    offset += (size_t)written;
  }
  return LS200_STATUS_OK;
}

static int consume_reader_output(int descriptor,
                                 const ls200_mutable_bytes *request_bytes,
                                 const ls200_preview_access_unit *unit,
                                 ls200_preview_flv_mux *mux,
                                 preview_output *output) {
  if (request_bytes->length != 0U &&
      !send_all(descriptor, (ls200_bytes){request_bytes->data,
                                         request_bytes->length})) return 0;
  if (unit->annex_b.length != 0U &&
      ls200_preview_flv_mux_write(mux, unit, write_flv, output) !=
          LS200_STATUS_OK) return 0;
  return 1;
}

static int pump_reader(int descriptor, ls200_preview_reader *reader,
                       ls200_preview_flv_mux *mux, preview_output *output,
                       ls200_bytes input) {
  uint8_t request_storage[LS200_PREVIEW_REQUEST_BYTES];
  ls200_mutable_bytes request_bytes = {
      .data = request_storage,
      .capacity = sizeof(request_storage),
      .length = 0U,
  };
  ls200_preview_access_unit unit;
  ls200_status status = ls200_preview_reader_push(reader, input,
                                                  &request_bytes, &unit);
  while (status == LS200_STATUS_OK) {
    if (!consume_reader_output(descriptor, &request_bytes, &unit, mux, output))
      return 0;
    status = ls200_preview_reader_drain(reader, &request_bytes, &unit);
  }
  return status == LS200_STATUS_AGAIN;
}

static int open_preview_source(ls200_fastcgi_server *server,
                               ls200_preview_reader **out_reader,
                               int *out_descriptor) {
  uint8_t initial_request[LS200_PREVIEW_REQUEST_BYTES];
  ls200_preview_reader_config reader_config;
  ls200_mutable_bytes request_bytes = {
      .data = initial_request,
      .capacity = sizeof(initial_request),
      .length = 0U,
  };
  *out_reader = NULL;
  *out_descriptor = -1;
  (void)memset(&reader_config, 0, sizeof(reader_config));
  if (inet_pton(AF_INET, server->gateway.config.preview_rtsp_ipv4,
                reader_config.target_ipv4) != 1) return 0;
  reader_config.target_port = server->gateway.config.preview_rtsp_port;
  reader_config.maximum_access_unit_bytes = LS200_PREVIEW_ACCESS_UNIT_BYTES;
  if (ls200_preview_reader_create(&reader_config, out_reader) != LS200_STATUS_OK)
    return 0;
  *out_descriptor = connect_preview_socket(&server->gateway.config);
  if (*out_descriptor >= 0 &&
      ls200_preview_reader_start(*out_reader, &request_bytes) ==
          LS200_STATUS_OK &&
      send_all(*out_descriptor,
               (ls200_bytes){request_bytes.data, request_bytes.length})) {
    return 1;
  }
  if (*out_descriptor >= 0) (void)close(*out_descriptor);
  *out_descriptor = -1;
  ls200_preview_reader_destroy(*out_reader);
  *out_reader = NULL;
  return 0;
}

static int wait_for_preview_input(int descriptor) {
  struct pollfd poll_descriptor = {descriptor, POLLIN, 0};
  int result;
  do {
    result = poll(&poll_descriptor, 1U, LS200_PREVIEW_LEASE_POLL_MS);
  } while (result < 0 && errno == EINTR);
  if (result <= 0) return result;
  if ((poll_descriptor.revents & POLLIN) != 0) return 1;
  return -1;
}

static void pump_preview_connection(
    FCGX_Request *request, ls200_fastcgi_server *server,
    const ls200_gateway_preview_lease *lease, int descriptor,
    ls200_preview_reader *reader, ls200_preview_flv_mux *mux,
    preview_output *output) {
  uint8_t input[8192];
  while (lease_is_valid(server, lease)) {
    int readiness = wait_for_preview_input(descriptor);
    ssize_t received;
    if (readiness < 0) return;
    if (readiness == 0) continue;
    received = recv(descriptor, input, sizeof(input), 0);
    if (received > 0) {
      if (!pump_reader(descriptor, reader, mux, output,
                       (ls200_bytes){input, (size_t)received})) return;
      if (output->headers_written && FCGX_FFlush(request->out) != 0) return;
      continue;
    }
    if (received == 0) {
      (void)ls200_preview_reader_eof(reader);
      return;
    }
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return;
  }
}

static void stream_preview(FCGX_Request *request, ls200_fastcgi_server *server,
                           const ls200_gateway_preview_lease *lease) {
  ls200_preview_reader *reader = NULL;
  ls200_preview_flv_mux mux;
  preview_output output = {request, 0};
  int descriptor = -1;
  if (open_preview_source(server, &reader, &descriptor)) {
    ls200_preview_flv_mux_init(&mux);
    pump_preview_connection(request, server, lease, descriptor, reader, &mux,
                            &output);
  }
  if (descriptor >= 0) (void)close(descriptor);
  ls200_preview_reader_destroy(reader);
  if (!output.headers_written) {
    write_preview_error(request, 503U, "PREVIEW_SOURCE_UNAVAILABLE",
                        "local preview source is unavailable");
  }
}

void ls200_fastcgi_handle_preview(FCGX_Request *request,
                                  ls200_fastcgi_server *server,
                                  const ls200_gateway_request *api_request) {
  ls200_gateway_preview_lease lease;
  ls200_gateway_response response;
  int reserved = reserve_preview(server, api_request, &lease, &response);
  if (reserved == 0) {
    write_gateway_response(request, &response);
    return;
  }
  if (reserved < 0) {
    write_preview_error(request, 409U, "PREVIEW_SOURCE_BUSY",
                        "local preview source is already in use");
    return;
  }
  stream_preview(request, server, &lease);
  release_preview(server);
}
