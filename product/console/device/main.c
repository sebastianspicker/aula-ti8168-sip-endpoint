#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif defined(__linux__)
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "server.h"
#include "transport.h"
#include "jobs.h"
#include "credentials_store.h"
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

static aula_device_jobs jobs;

static json_t *read_jobs(void) {
  return aula_device_jobs_list(&jobs);
}

static json_t *unavailable_status(void) {
  return json_pack("{s:i,s:s,s:s}", "revision", 1,
      "recording", "unknown", "streaming", "unknown");
}


static int open_state_directory(void) {
  const char *const parts[] = {"run", "aula-state", "device"};
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
      strcmp(address.sun_path, AULA_DEVICE_SOCKET) == 0 &&
      getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &option_size) == 0 &&
      listening && lstat(AULA_DEVICE_SOCKET, &info) == 0 &&
      S_ISSOCK(info.st_mode) && info.st_uid == 0 && info.st_gid == gateway_group &&
      (info.st_mode & 0777) == 0660;
}

static int prepare_listener(int argc, char **argv, uid_t *gateway_uid) {
  struct passwd *gateway = getpwnam("aula-gateway");
  if (geteuid() != 0 || gateway == NULL || gateway->pw_uid == 0 || argc != 2) return 0;
  *gateway_uid = gateway->pw_uid;
  gid_t gateway_group = gateway->pw_gid;
  if (strcmp(argv[1], "--launch") == 0 && aula_device_listener(gateway_group) != 3) return 0;
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
  if (directory < 0 || !aula_device_jobs_open(&jobs, directory) ||
      !aula_device_credentials_open(directory)) {
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
      aula_device_jobs_close(&jobs);
      return 1;
    }
    if (fcntl(client, F_SETFL, O_NONBLOCK) == 0 &&
        fcntl(client, F_SETFD, FD_CLOEXEC) == 0)
      (void)aula_device_serve_commands(client, (uint32_t)gateway_uid,
          unavailable_status, read_jobs, aula_device_dispatch);
    close(client);
  }
}
