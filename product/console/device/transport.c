#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

uint64_t aula_device_clock(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

int aula_device_peer(int fd, uint32_t owner) {
#if defined(__linux__)
  struct ucred credentials;
  socklen_t size = sizeof(credentials);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
      size == sizeof(credentials) && credentials.uid == (uid_t)owner;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  uid_t uid;
  gid_t gid;
  return getpeereid(fd, &uid, &gid) == 0 && uid == (uid_t)owner;
#else
  (void)fd; (void)owner;
  return 0;
#endif
}

static int device_wait(int fd, short events, uint64_t deadline) {
  struct pollfd item = {fd, events, 0};
  for (;;) {
    uint64_t now = aula_device_clock();
    int result;
    if (now == 0 || now >= deadline || deadline - now > INT_MAX) return 0;
    result = poll(&item, 1, (int)(deadline - now));
    if (result < 0 && errno == EINTR) continue;
    return result > 0 && (item.revents & events) != 0;
  }
}

int aula_device_transfer(int fd, void *buffer, size_t length, int writing,
                          uint64_t deadline) {
  size_t offset = 0;
  int send_flags = 0;
#if defined(MSG_NOSIGNAL)
  send_flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
  int enabled = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
    return 0;
#else
  if (writing) return 0;
#endif
  while (offset < length) {
    ssize_t count;
    if (!device_wait(fd, writing ? POLLOUT : POLLIN, deadline)) return 0;
    count = writing ? send(fd, (char *)buffer + offset, length - offset, send_flags) :
        recv(fd, (char *)buffer + offset, length - offset, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) return 0;
    offset += (size_t)count;
  }
  return 1;
}

/* Reject symlinked ancestors and writable directories. /var/run may be a
 * platform alias: callers use the canonical /run path on those systems. */
static int device_path_safe(const char *path, uint32_t owner) {
  char copy[108], *part, *next;
  int directory, valid = 0;
  struct stat info;
  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) return 0;
  memcpy(copy, path + 1, strlen(path));
  directory = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory < 0) return 0;
  part = copy;
  while ((next = strchr(part, '/')) != NULL) {
    int child;
    *next = '\0';
    child = openat(directory, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close(directory);
    if (child < 0) return 0;
    directory = child;
    if (fstat(directory, &info) != 0 || (info.st_mode & 0022) != 0 ||
        (info.st_uid != 0 && info.st_uid != (uid_t)owner)) goto done;
    part = next + 1;
  }
  valid = fstatat(directory, part, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISSOCK(info.st_mode) && info.st_uid == (uid_t)owner &&
      (info.st_mode & 0002) == 0;
done:
  close(directory);
  return valid;
}

static int device_connected(int fd, uint32_t owner, uint64_t deadline) {
  int error = -1;
  socklen_t size = sizeof(error);
  return device_wait(fd, POLLOUT, deadline) &&
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
      size == sizeof(error) && error == 0 && aula_device_peer(fd, owner);
}

int aula_device_connect(const char *path, uint32_t owner, unsigned timeout_ms) {
  struct sockaddr_un address;
  int fd;
  uint64_t now = aula_device_clock();
  if (path == NULL || strlen(path) >= sizeof(address.sun_path) ||
      !now || timeout_ms == 0 || timeout_ms > 5000 ||
      !device_path_safe(path, owner)) return -1;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1);
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    goto failed;
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0 &&
      errno != EINPROGRESS) goto failed;
  if (!device_connected(fd, owner, now + timeout_ms)) goto failed;
  return fd;
failed:
  close(fd);
  return -1;
}
