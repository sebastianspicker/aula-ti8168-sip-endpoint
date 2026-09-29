#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "launcher.h"
#include "transport.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int listener_lock = -1;

static int runtime_directory_safe(void) {
  struct stat info;
  return geteuid() == 0 && lstat("/run", &info) == 0 && S_ISDIR(info.st_mode) &&
      info.st_uid == 0 && (info.st_mode & 0022) == 0;
}

static int listener_directory(gid_t group) {
  struct stat info;
  int root, directory;
  if (!runtime_directory_safe()) return -1;
  root = open("/run", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0) return -1;
  int created = mkdirat(root, "aula-device", 0700) == 0;
  if (!created && errno != EEXIST) { close(root); return -1; }
  directory = openat(root, "aula-device", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  close(root);
  if (directory < 0) return -1;
  if (created && (fchown(directory, 0, group) != 0 || fchmod(directory, 0750) != 0)) goto failed;
  if (fstat(directory, &info) == 0 && info.st_uid == 0 && info.st_gid == group &&
      (info.st_mode & 0777) == 0750) return directory;
failed:
  close(directory);
  return -1;
}

static int lock_listener(int directory) {
  struct stat info;
  int fd = openat(directory, "listener.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (fd < 0) return 0;
  int okay = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == 0 &&
      info.st_nlink == 1 && (info.st_mode & 0077) == 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
  if (okay) listener_lock = fcntl(fd, F_DUPFD_CLOEXEC, 4);
  close(fd);
  return okay && listener_lock >= 4;
}

static int recover_listener(int directory, gid_t group, struct sockaddr_un *address) {
  struct stat before, after;
  int fd, result, saved_errno;
  if (fstatat(directory, "control.sock", &before, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
  if (!S_ISSOCK(before.st_mode) || before.st_uid != 0 || before.st_gid != group ||
      (before.st_mode & 0777) != 0660) return 0;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) { close(fd); return 0; }
  result = connect(fd, (struct sockaddr *)address, sizeof(*address));
  saved_errno = errno;
  close(fd);
  if (result == 0 || saved_errno != ECONNREFUSED) return 0;
  return fstatat(directory, "control.sock", &after, AT_SYMLINK_NOFOLLOW) == 0 &&
      before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      unlinkat(directory, "control.sock", 0) == 0;
}

static int activate_socket(int directory, int fd, gid_t group) {
  return fchownat(directory, "control.sock", 0, group, AT_SYMLINK_NOFOLLOW) == 0 &&
      fchmodat(directory, "control.sock", 0660, 0) == 0 && listen(fd, 16) == 0;
}

int aula_device_listener(gid_t gateway_group) {
  struct sockaddr_un address;
  int directory = listener_directory(gateway_group), fd = -1, okay = 0;
  if (directory < 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, AULA_DEVICE_SOCKET);
  if (!lock_listener(directory) || !recover_listener(directory, gateway_group, &address)) goto done;
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) goto done;
  mode_t previous = umask(0077);
  okay = bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
  umask(previous);
  if (okay) okay = activate_socket(directory, fd, gateway_group);
done:
  close(directory);
  if (okay && fd != 3) okay = dup2(fd, 3) == 3;
  if (fd >= 0 && (fd != 3 || !okay)) close(fd);
  if (!okay && listener_lock >= 0) { close(listener_lock); listener_lock = -1; }
  return okay ? 3 : -1;
}
