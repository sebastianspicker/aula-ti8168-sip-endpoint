#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif defined(__linux__)
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "oem.h"
#include "transport.h"
#include "jobs.h"
#include "credentials.h"
#include "launcher.h"
#include "commands.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static ls200_device_jobs jobs;

static json_t *read_jobs(void) {
  return ls200_device_jobs_list(&jobs);
}


static int open_state_directory(void) {
  const char *const parts[] = {"run", "ls200-zoom-state", "device"};
  int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  struct stat info;
  for (size_t i = 0; directory >= 0 && i < 3; ++i) {
    int child = openat(directory, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(directory);
    if (child < 0) return -1;
    if (fstat(child, &info) != 0 || (info.st_mode & 0022) != 0 ||
        (info.st_uid != 0 && info.st_uid != geteuid())) {
      close(child); return -1;
    }
    directory = child;
  }
  return directory;
}

/* Socket activation keeps creation, stale-path recovery, permissions and
 * service identity in the supervisor. Never unlink an operator-owned socket. */
static int inherited_listener_valid(int fd, gid_t gateway_group) {
  struct sockaddr_un address;
  struct stat info;
  socklen_t size = sizeof(address), option_size = sizeof(int);
  int listening = 0;
  memset(&address, 0, sizeof(address));
  return getsockname(fd, (struct sockaddr *)&address, &size) == 0 &&
      address.sun_family == AF_UNIX &&
      memchr(address.sun_path, '\0', sizeof(address.sun_path)) != NULL &&
      strcmp(address.sun_path, LS200_DEVICE_SOCKET) == 0 &&
      getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &option_size) == 0 &&
      listening && lstat(LS200_DEVICE_SOCKET, &info) == 0 &&
      S_ISSOCK(info.st_mode) && info.st_uid == 0 && info.st_gid == gateway_group &&
      (info.st_mode & 0777) == 0660;
}

static int prepare_listener(int argc, char **argv, uid_t *gateway_uid) {
  struct passwd *gateway = getpwnam("ls200-gateway");
  if (geteuid() != 0 || gateway == NULL || gateway->pw_uid == 0 || argc != 2) return 0;
  *gateway_uid = gateway->pw_uid;
  gid_t gateway_group = gateway->pw_gid;
  if (strcmp(argv[1], "--launch") == 0 && ls200_device_listener(gateway_group) != 3) return 0;
  if ((strcmp(argv[1], "--socket-activation") != 0 && strcmp(argv[1], "--launch") != 0) ||
      !inherited_listener_valid(3, gateway_group)) {
    fputs("device-control: requires an owned listener on descriptor 3\n", stderr);
    return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  int directory;
  uid_t gateway_uid;
  if (!prepare_listener(argc, argv, &gateway_uid)) return 2;
  directory = open_state_directory();
  if (directory < 0 || !ls200_device_jobs_open(&jobs, directory) ||
      !ls200_device_credentials_open(directory)) {
    if (directory >= 0) close(directory);
    fputs("device-control: protected job state is unavailable or incompatible\n", stderr);
    return 2;
  }
  close(directory);
  signal(SIGPIPE, SIG_IGN);
  for (;;) {
    int client = accept(3, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) continue;
      ls200_device_jobs_close(&jobs);
      return 1;
    }
    if (fcntl(client, F_SETFL, O_NONBLOCK) == 0 &&
        fcntl(client, F_SETFD, FD_CLOEXEC) == 0)
      (void)ls200_device_serve_commands(client, (uint32_t)gateway_uid,
          ls200_device_oem_status, read_jobs, ls200_device_dispatch);
    close(client);
  }
}
