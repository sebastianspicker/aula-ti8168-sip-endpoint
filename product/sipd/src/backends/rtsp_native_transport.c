#include "rtsp_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int ls200_rtsp_native_enabled(const native_context *context) {
  const char *name;
  const char *enabled;
  if (context == NULL) return 0;
  name = ls200_rtsp_ipv4_is_loopback(context->video_uri.address) != 0 ?
      "LS200_SIPD_ENABLE_LOCAL_RTSP" : "LS200_SIPD_ENABLE_AUTHORIZED_RTSP";
  enabled = getenv(name);
  return enabled != NULL && strcmp(enabled, "1") == 0;
}

ls200_status ls200_rtsp_native_send(native_context *context, const char *method,
                                    const char *uri, const char *headers,
                                    native_state next_state) {
  int written;
  if (context == NULL || method == NULL || uri == NULL || headers == NULL ||
      context->pending_length != 0U) return LS200_STATUS_STATE_ERROR;
  written = snprintf(context->pending, sizeof(context->pending),
                     "%s %s RTSP/1.0\r\nCSeq: %u\r\n%s\r\n", method, uri,
                     (unsigned int)context->cseq, headers);
  if (written < 0 || (size_t)written >= sizeof(context->pending)) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  context->pending_length = (size_t)written;
  context->pending_offset = 0U;
  context->pending_state = next_state;
  context->cseq++;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_flush(native_context *context) {
  ssize_t sent;
  if (context == NULL || context->fd < 0 ||
      context->pending_offset > context->pending_length) return LS200_STATUS_STATE_ERROR;
  if (context->pending_length == 0U) return LS200_STATUS_OK;
  sent = send(context->fd, context->pending + context->pending_offset,
              context->pending_length - context->pending_offset, MSG_NOSIGNAL);
  if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    return LS200_STATUS_AGAIN;
  }
  if (sent <= 0) return LS200_STATUS_IO_ERROR;
  context->pending_offset += (size_t)sent;
  if (context->pending_offset != context->pending_length) return LS200_STATUS_AGAIN;
  context->pending_offset = 0U;
  context->pending_length = 0U;
  context->state = context->pending_state;
  return LS200_STATUS_OK;
}

static ls200_status native_open_udp_socket(uint16_t port, int rtp, int *out_fd) {
  struct sockaddr_in address;
  const int receive_bytes = 256 * 1024;
  int fd;
  int flags;
  if (out_fd == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return LS200_STATUS_IO_ERROR;
  /* A camera keyframe burst exceeds the appliance's default 64 KiB queue. */
  if (rtp != 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_bytes,
                             (socklen_t)sizeof(receive_bytes)) != 0) {
    (void)close(fd);
    return LS200_STATUS_IO_ERROR;
  }
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    (void)close(fd);
    return LS200_STATUS_IO_ERROR;
  }
  (void)memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = (in_port_t)htons(port);
  if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
    (void)close(fd);
    return LS200_STATUS_IO_ERROR;
  }
  *out_fd = fd;
  return LS200_STATUS_OK;
}

static ls200_status native_open_udp_pair(int *out_rtp_fd, int *out_rtcp_fd,
                                         uint16_t *out_rtp_port) {
  struct sockaddr_in address;
  unsigned int attempt;
  int rtp_fd = -1;
  int rtcp_fd = -1;
  uint16_t port;
  if (out_rtp_fd == NULL || out_rtcp_fd == NULL || out_rtp_port == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  for (attempt = 0U; attempt < LS200_RTSP_NATIVE_MAX_UDP_OPEN_ATTEMPTS;
       ++attempt) {
    socklen_t length = sizeof(address);
    if (native_open_udp_socket(0U, 1, &rtp_fd) != LS200_STATUS_OK) {
      return LS200_STATUS_IO_ERROR;
    }
    (void)memset(&address, 0, sizeof(address));
    if (getsockname(rtp_fd, (struct sockaddr *)&address, &length) != 0 ||
        ntohs(address.sin_port) == 65535U) {
      (void)close(rtp_fd);
      return LS200_STATUS_IO_ERROR;
    }
    port = (uint16_t)ntohs(address.sin_port);
    if (native_open_udp_socket((uint16_t)(port + 1U), 0, &rtcp_fd) ==
        LS200_STATUS_OK) {
      *out_rtp_fd = rtp_fd;
      *out_rtcp_fd = rtcp_fd;
      *out_rtp_port = port;
      return LS200_STATUS_OK;
    }
    (void)close(rtp_fd);
  }
  return LS200_STATUS_IO_ERROR;
}

ls200_status ls200_rtsp_native_connect(native_context *context) {
  struct sockaddr_in address;
  int fd;
  int flags;
  if (context == NULL || ls200_rtsp_native_enabled(context) == 0) {
    return LS200_STATUS_PERMISSION_DENIED;
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return LS200_STATUS_IO_ERROR;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    (void)close(fd);
    return LS200_STATUS_IO_ERROR;
  }
#if defined(SO_NOSIGPIPE)
  {
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   (socklen_t)sizeof(enabled)) != 0) {
      (void)close(fd);
      return LS200_STATUS_IO_ERROR;
    }
  }
#endif
  (void)memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  (void)memcpy(&address.sin_addr, context->video_uri.address,
               sizeof(context->video_uri.address));
  address.sin_port = (in_port_t)htons(context->video_uri.port);
  if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 &&
      errno != EINPROGRESS) {
    (void)close(fd);
    return LS200_STATUS_IO_ERROR;
  }
  context->fd = fd;
  context->state = NATIVE_CONNECTING;
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_start_request(native_context *context) {
  int error = 0;
  socklen_t length = sizeof(error);
  if (context == NULL || context->state != NATIVE_CONNECTING) {
    return LS200_STATUS_STATE_ERROR;
  }
  if (getsockopt(context->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
    return LS200_STATUS_IO_ERROR;
  }
  if (error == EINPROGRESS || error == EALREADY) return LS200_STATUS_AGAIN;
  if (error != 0) return LS200_STATUS_IO_ERROR;
  return ls200_rtsp_native_send(context, "OPTIONS", context->video_uri.text,
                                "", NATIVE_OPTIONS);
}

ls200_status ls200_rtsp_native_transport_header(native_context *context,
                                                 int *rtp_fd, int *rtcp_fd,
                                                 unsigned int first_channel,
                                                 char *headers, size_t capacity) {
  uint16_t port;
  int written;
  if (context->use_udp != 0) {
    if (native_open_udp_pair(rtp_fd, rtcp_fd, &port) != LS200_STATUS_OK) {
      return LS200_STATUS_IO_ERROR;
    }
    written = snprintf(headers, capacity,
                       "Transport: RTP/AVP;unicast;client_port=%u-%u\r\n",
                       port, (unsigned int)port + 1U);
  } else {
    written = snprintf(headers, capacity,
                       "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                       first_channel, first_channel + 1U);
  }
  return written < 0 || (size_t)written >= capacity ?
      LS200_STATUS_LIMIT_EXCEEDED : LS200_STATUS_OK;
}

static int native_source_matches(const native_context *context,
                                 const struct sockaddr_in *source,
                                 uint16_t *locked_port) {
  uint16_t port;
  if (context == NULL || source == NULL || locked_port == NULL ||
      source->sin_family != AF_INET ||
      memcmp(&source->sin_addr, context->video_uri.address,
             sizeof(context->video_uri.address)) != 0) return 0;
  port = (uint16_t)ntohs(source->sin_port);
  if (port == 0U) return 0;
  if (*locked_port == 0U) {
    *locked_port = port;
    return 1;
  }
  return *locked_port == port;
}

static ls200_status native_drain_udp_fd(native_context *context, int fd,
                                        uint16_t *locked_port, int video,
                                        int rtcp) {
  uint8_t packet[LS200_SIPD_MAX_RTP_PACKET_BYTES];
  struct sockaddr_in source;
  ls200_status status = LS200_STATUS_AGAIN;
  unsigned int count;
  for (count = 0U;
       count < LS200_RTSP_NATIVE_MAX_UDP_DATAGRAMS_PER_CHANNEL; ++count) {
    socklen_t source_length = sizeof(source);
    ssize_t received = recvfrom(fd, packet, sizeof(packet), 0,
                                (struct sockaddr *)&source, &source_length);
    if (received < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return status;
      return LS200_STATUS_IO_ERROR;
    }
    if (received == 0) return status;
    if (native_source_matches(context, &source, locked_port) == 0) {
      context->health.frames_dropped++;
      continue;
    }
    if (rtcp != 0) {
      if ((size_t)received > LS200_SIPD_MAX_RTCP_PACKET_BYTES) {
        context->health.frames_dropped++;
      }
      continue;
    }
    status = video != 0 ? ls200_rtsp_native_push_video(
        context, (ls200_bytes){packet, (size_t)received}) :
        ls200_rtsp_native_push_audio(context, (ls200_bytes){packet, (size_t)received});
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
    if (context->video_ready != 0 || context->audio_ready != 0) return status;
  }
  return status;
}

static ls200_status native_drain_channel(native_context *context, int fd,
                                         uint16_t *locked_port, int video,
                                         int rtcp) {
  ls200_status status;
  if (fd < 0) return LS200_STATUS_AGAIN;
  if (rtcp == 0 && ((video != 0 && context->video_ready != 0) ||
                    (video == 0 && context->audio_ready != 0))) {
    return LS200_STATUS_AGAIN;
  }
  status = native_drain_udp_fd(context, fd, locked_port, video, rtcp);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  return status;
}

ls200_status ls200_rtsp_native_drain_udp(native_context *context) {
  ls200_status result = LS200_STATUS_AGAIN;
  ls200_status status;
  if (context->use_udp == 0 || context->state != NATIVE_STREAMING) {
    return LS200_STATUS_AGAIN;
  }
  status = native_drain_channel(context, context->video_udp_fd,
                                &context->video_source_port, 1, 0);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  if (status == LS200_STATUS_OK) result = status;
  status = native_drain_channel(context, context->audio_udp_fd,
                                &context->audio_source_port, 0, 0);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  if (status == LS200_STATUS_OK) result = status;
  status = native_drain_channel(context, context->video_rtcp_fd,
                                &context->video_rtcp_source_port, 1, 1);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  status = native_drain_channel(context, context->audio_rtcp_fd,
                                &context->audio_rtcp_source_port, 0, 1);
  if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) return status;
  return result;
}

void ls200_rtsp_native_close_transports(native_context *context) {
  if (context == NULL) return;
  if (context->fd >= 0) (void)close(context->fd);
  if (context->video_udp_fd >= 0) (void)close(context->video_udp_fd);
  if (context->video_rtcp_fd >= 0) (void)close(context->video_rtcp_fd);
  if (context->audio_udp_fd >= 0) (void)close(context->audio_udp_fd);
  if (context->audio_rtcp_fd >= 0) (void)close(context->audio_rtcp_fd);
  context->fd = -1;
  context->video_udp_fd = -1;
  context->video_rtcp_fd = -1;
  context->audio_udp_fd = -1;
  context->audio_rtcp_fd = -1;
}

ls200_status ls200_rtsp_native_test_open_udp(ls200_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  uint16_t ignored_port;
  if (c == NULL || c->opened == 0) return LS200_STATUS_INVALID_ARGUMENT;
  ls200_rtsp_native_close_transports(c);
  c->use_udp = 1;
  if (native_open_udp_pair(&c->video_udp_fd, &c->video_rtcp_fd, &ignored_port) != LS200_STATUS_OK ||
      native_open_udp_pair(&c->audio_udp_fd, &c->audio_rtcp_fd, &ignored_port) != LS200_STATUS_OK) {
    ls200_rtsp_native_close_transports(c); return LS200_STATUS_IO_ERROR;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_test_udp_port(const ls200_media_backend *backend,
                                              int video, int rtcp, uint16_t *out_port) {
  const native_context *c = backend == NULL ? NULL : backend->context;
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  int fd;
  if (c == NULL || out_port == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  fd = video != 0 ? (rtcp != 0 ? c->video_rtcp_fd : c->video_udp_fd) :
      (rtcp != 0 ? c->audio_rtcp_fd : c->audio_udp_fd);
  if (fd < 0 || getsockname(fd, (struct sockaddr *)&address, &length) != 0) return LS200_STATUS_STATE_ERROR;
  *out_port = (uint16_t)ntohs(address.sin_port); return LS200_STATUS_OK;
}

ls200_status ls200_rtsp_native_test_drain_udp(ls200_media_backend *backend) {
  native_context *c = backend == NULL ? NULL : backend->context;
  return c == NULL ? LS200_STATUS_INVALID_ARGUMENT : ls200_rtsp_native_drain_udp(c);
}

ls200_status ls200_rtsp_native_test_dispatch_udp(ls200_media_backend *backend,
                                                  int video, int rtcp,
                                                  uint16_t source_port,
                                                  ls200_bytes packet) {
  native_context *c = backend == NULL ? NULL : backend->context;
  struct sockaddr_in source;
  uint16_t *locked_port;
  if (c == NULL || source_port == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&source, 0, sizeof(source)); source.sin_family = AF_INET;
  (void)memcpy(&source.sin_addr, c->video_uri.address, sizeof(c->video_uri.address));
  source.sin_port = (in_port_t)htons(source_port);
  locked_port = video != 0 ? (rtcp != 0 ? &c->video_rtcp_source_port : &c->video_source_port) :
      (rtcp != 0 ? &c->audio_rtcp_source_port : &c->audio_source_port);
  if (native_source_matches(c, &source, locked_port) == 0) { c->health.frames_dropped++; return LS200_STATUS_AGAIN; }
  if (rtcp != 0) return packet.length <= LS200_SIPD_MAX_RTCP_PACKET_BYTES ? LS200_STATUS_AGAIN : LS200_STATUS_LIMIT_EXCEEDED;
  return video != 0 ? ls200_rtsp_native_push_video(c, packet) : ls200_rtsp_native_push_audio(c, packet);
}

ls200_status ls200_rtsp_native_test_fail(ls200_media_backend *backend,
                                         ls200_status cause) {
  native_context *c = backend == NULL ? NULL : backend->context;
  return c == NULL ? LS200_STATUS_INVALID_ARGUMENT : ls200_rtsp_native_schedule_reconnect(c, cause);
}
