#define _POSIX_C_SOURCE 200809L
#include "receive_monitor_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static aula_status spawn_decoder(aula_monitor_worker *worker,
                                   const char *path, int input, int video) {
  const char *const video_args[] = {path, "-nostdin", "-loglevel", "error",
      "-threads", "1", "-probesize", "32768", "-analyzeduration", "0",
      "-skip_frame", "nokey", "-f", "h264", "-i", "pipe:0",
      "-an", "-f", "null", "-", NULL};
  const char *const audio_args[] = {path, "-nostdin", "-loglevel", "error",
      "-threads", "1", "-f", "s16le", "-ar", "48000", "-ac", "2",
      "-i", "pipe:0", "-vn", "-f", "null", "-", NULL};
  const char *const environment[] = {"LANG=C", "LC_ALL=C", NULL};
  aula_child_process_config config;
  (void)memset(&config, 0, sizeof(config));
  config.executable_path = path;
  config.argv = video ? video_args : audio_args;
  config.environment = environment;
  config.stdin_fd = input;
  config.stdout_fd = -1;
  config.stderr_fd = -1;
  config.expected_uid = (uint32_t)geteuid();
  config.expected_gid = (uint32_t)getegid();
  config.require_expected_identity = 1;
  config.require_non_root_identity = 1;
  config.close_unlisted_fds = 1;
  config.limits = (aula_child_process_limits){86400U, 134217728U, 0U, 32U, 32U, 0U};
  return aula_platform_spawn_child(&config, &worker->child);
}

void aula_monitor_worker_stop(aula_monitor_worker *worker) {
  uint64_t now = 0U;
  if (worker->input_fd >= 0) (void)close(worker->input_fd);
  worker->input_fd = -1;
  if (worker->child.pid > 0) {
    (void)aula_platform_monotonic_now(&now);
    (void)aula_platform_stop_child(&worker->child,
        (aula_deadline){now + UINT64_C(500000000)});
  }
  free(worker->pending);
  worker->pending = NULL;
  worker->length = 0U;
  worker->offset = 0U;
}

aula_status aula_monitor_worker_start(aula_monitor_worker *worker,
                                         const char *path, int video) {
  int descriptors[2];
  aula_status status;
  if (worker->child.pid > 0 || worker->input_fd >= 0) return AULA_STATUS_STATE_ERROR;
  worker->capacity = video ? AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES : 65536U;
  worker->pending = malloc(worker->capacity);
  if (worker->pending == NULL) return AULA_STATUS_INTERNAL_ERROR;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
    aula_monitor_worker_stop(worker);
    return AULA_STATUS_IO_ERROR;
  }
  worker->input_fd = descriptors[1];
#if defined(SO_NOSIGPIPE)
  {
    int enabled = 1;
    if (setsockopt(descriptors[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
      (void)close(descriptors[0]);
      aula_monitor_worker_stop(worker);
      return AULA_STATUS_IO_ERROR;
    }
  }
#endif
  if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0 ||
      fcntl(descriptors[1], F_SETFL, O_NONBLOCK) < 0) {
    (void)close(descriptors[0]);
    aula_monitor_worker_stop(worker);
    return AULA_STATUS_IO_ERROR;
  }
  status = spawn_decoder(worker, path, descriptors[0], video);
  (void)close(descriptors[0]);
  if (status != AULA_STATUS_OK) aula_monitor_worker_stop(worker);
  return status;
}

aula_status aula_monitor_worker_flush(aula_monitor_worker *worker) {
  int exit_status;
  unsigned attempts = 0U;
  if (worker->input_fd < 0 || worker->child.pid <= 0 ||
      aula_platform_reap_child(&worker->child, &exit_status) != AULA_STATUS_AGAIN)
    return AULA_STATUS_IO_ERROR;
  while (worker->offset < worker->length && attempts++ < 8U) {
    ssize_t count = send(worker->input_fd, worker->pending + worker->offset,
                           worker->length - worker->offset,
#if defined(MSG_NOSIGNAL)
                           MSG_NOSIGNAL
#else
                           0
#endif
                           );
    if (count > 0) worker->offset += (size_t)count;
    else if (count < 0 && errno == EINTR) continue;
    else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    else return AULA_STATUS_IO_ERROR;
  }
  if (worker->offset == worker->length) worker->offset = worker->length = 0U;
  return AULA_STATUS_OK;
}

aula_status aula_monitor_worker_submit(aula_monitor_worker *worker, aula_bytes data) {
  aula_status status = aula_monitor_worker_flush(worker);
  size_t pending = worker->length - worker->offset;
  if (status != AULA_STATUS_OK) return status;
  if (data.length > worker->capacity - pending) return AULA_STATUS_LIMIT_EXCEEDED;
  if (worker->offset != 0U)
    (void)memmove(worker->pending, worker->pending + worker->offset, pending);
  worker->offset = 0U;
  worker->length = pending;
  (void)memcpy(worker->pending + pending, data.data, data.length);
  worker->length += data.length;
  return aula_monitor_worker_flush(worker);
}
