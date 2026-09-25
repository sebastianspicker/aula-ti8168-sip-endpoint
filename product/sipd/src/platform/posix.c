#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "platform_private.h"
#include "ls200_sipd/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#if defined(LS200_SIPD_HAVE_GETRANDOM) && LS200_SIPD_HAVE_GETRANDOM && !defined(LS200_SIPD_TARGET_ARM_EABI5)
#include <sys/random.h>
#endif

static volatile sig_atomic_t platform_signal_request = LS200_SIGNAL_NONE;

#if defined(__linux__)
static ls200_status parse_linux_birth_token(char *buffer, uint64_t *out_token) {
  char *cursor = strrchr(buffer, ')');
  char *end;
  unsigned int field = 3U;
  if (cursor == NULL || cursor[1] != ' ') return LS200_STATUS_INVALID_DATA;
  cursor += 2;
  while (*cursor != '\0') {
    char *token = cursor;
    while (*cursor != '\0' && *cursor != ' ') ++cursor;
    if (*cursor == ' ') { *cursor = '\0'; ++cursor; while (*cursor == ' ') ++cursor; }
    if (field == 22U) { unsigned long long value; errno = 0; value = strtoull(token, &end, 10); if (errno != 0 || end == token || *end != '\0' || value == 0ULL) return LS200_STATUS_INVALID_DATA; *out_token = (uint64_t)value; return LS200_STATUS_OK; }
    ++field;
  }
  return LS200_STATUS_INVALID_DATA;
}
static ls200_status linux_process_birth_token(pid_t pid, uint64_t *out_token) {
  char path[64]; char buffer[1024]; ssize_t count; int descriptor;
  int written = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return LS200_STATUS_LIMIT_EXCEEDED;
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return errno == ENOENT ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  do { count = read(descriptor, buffer, sizeof(buffer) - 1U); } while (count < 0 && errno == EINTR);
  (void)close(descriptor);
  if (count <= 0 || (size_t)count >= sizeof(buffer)) return LS200_STATUS_IO_ERROR;
  buffer[count] = '\0'; return parse_linux_birth_token(buffer, out_token);
}
#endif
#if defined(__APPLE__)
static ls200_status macos_process_birth_token(pid_t pid, uint64_t *out_token) {
  struct proc_bsdinfo info;
  int bytes = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &info, (int)sizeof(info));
  if (bytes != (int)sizeof(info)) return errno == ESRCH ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  if (info.pbi_start_tvsec == 0U && info.pbi_start_tvusec == 0U) return LS200_STATUS_INVALID_DATA;
  *out_token = ((uint64_t)info.pbi_start_tvsec * UINT64_C(1000000)) + (uint64_t)info.pbi_start_tvusec;
  return LS200_STATUS_OK;
}
#endif
ls200_status ls200_platform_process_birth_token(pid_t pid, uint64_t *out_token) {
  if (pid <= 0 || out_token == NULL) return LS200_STATUS_INVALID_ARGUMENT;
#if defined(__linux__)
  return linux_process_birth_token(pid, out_token);
#elif defined(__APPLE__)
  return macos_process_birth_token(pid, out_token);
#else
  return LS200_STATUS_UNSUPPORTED;
#endif
}

static ls200_status process_effective_identity(pid_t pid, uint32_t *out_uid, uint32_t *out_gid) {
  if (pid <= 0 || out_uid == NULL || out_gid == NULL) return LS200_STATUS_INVALID_ARGUMENT;
#if defined(__linux__)
  char path[64]; struct stat details; int written = snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return LS200_STATUS_LIMIT_EXCEEDED;
  if (stat(path, &details) != 0) return errno == ENOENT ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  if ((uint64_t)details.st_uid > UINT32_MAX || (uint64_t)details.st_gid > UINT32_MAX) return LS200_STATUS_LIMIT_EXCEEDED;
  *out_uid = (uint32_t)details.st_uid; *out_gid = (uint32_t)details.st_gid; return LS200_STATUS_OK;
#elif defined(__APPLE__)
  struct proc_bsdinfo info; int bytes = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &info, (int)sizeof(info));
  if (bytes != (int)sizeof(info)) return errno == ESRCH ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  *out_uid = (uint32_t)info.pbi_uid; *out_gid = (uint32_t)info.pbi_gid; return LS200_STATUS_OK;
#else
  (void)pid; return LS200_STATUS_UNSUPPORTED;
#endif
}

ls200_status ls200_platform_monotonic_now(uint64_t *out_ns) {
  struct timespec now;
  if (out_ns == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0L || now.tv_nsec < 0L) return LS200_STATUS_IO_ERROR;
  *out_ns = ((uint64_t)now.tv_sec * UINT64_C(1000000000)) + (uint64_t)now.tv_nsec; return LS200_STATUS_OK;
}
ls200_status ls200_platform_ntp_now(uint64_t *out_ntp) {
  const uint64_t epoch = UINT64_C(2208988800); struct timespec now; uint64_t seconds; uint64_t fraction;
  if (out_ntp == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0L || now.tv_nsec < 0L || now.tv_nsec >= 1000000000L) return LS200_STATUS_IO_ERROR;
  if ((uint64_t)now.tv_sec > UINT64_MAX - epoch) return LS200_STATUS_LIMIT_EXCEEDED;
  seconds = (uint64_t)now.tv_sec + epoch; fraction = ((uint64_t)now.tv_nsec << 32U) / UINT64_C(1000000000);
  if (fraction > UINT32_MAX) return LS200_STATUS_INTERNAL_ERROR;
  *out_ntp = ((seconds & UINT64_C(0xffffffff)) << 32U) | fraction; return LS200_STATUS_OK;
}
static ls200_status fill_random_descriptor(int descriptor, ls200_mutable_bytes *output, size_t offset) {
  while (offset < output->capacity) { ssize_t count = read(descriptor, output->data + offset, output->capacity - offset); if (count > 0) { offset += (size_t)count; continue; } if (count < 0 && errno == EINTR) continue; return LS200_STATUS_IO_ERROR; }
  output->length = offset; return LS200_STATUS_OK;
}
static ls200_status fill_random_getrandom(ls200_mutable_bytes *output, size_t *out_offset) {
  size_t offset = 0U;
#if !defined(LS200_SIPD_HAVE_GETRANDOM) || !LS200_SIPD_HAVE_GETRANDOM || defined(LS200_SIPD_TARGET_ARM_EABI5)
  (void)output;
#endif
#if defined(LS200_SIPD_HAVE_GETRANDOM) && LS200_SIPD_HAVE_GETRANDOM && !defined(LS200_SIPD_TARGET_ARM_EABI5)
  while (offset < output->capacity) { ssize_t count = getrandom(output->data + offset, output->capacity - offset, 0); if (count > 0) { offset += (size_t)count; continue; } if (count < 0 && errno == EINTR) continue; if (count < 0 && (errno == ENOSYS || errno == EAGAIN)) break; return LS200_STATUS_IO_ERROR; }
#endif
  *out_offset = offset;
  return LS200_STATUS_OK;
}
ls200_status ls200_platform_random_bytes(ls200_mutable_bytes *output) {
  int descriptor; size_t offset; ls200_status status;
  if (output == NULL || output->data == NULL || output->capacity == 0U || output->length != 0U) return LS200_STATUS_INVALID_ARGUMENT;
  status = fill_random_getrandom(output, &offset);
  if (status != LS200_STATUS_OK) return status;
  if (offset == output->capacity) { output->length = offset; return LS200_STATUS_OK; }
  descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (descriptor < 0) return LS200_STATUS_IO_ERROR;
  status = fill_random_descriptor(descriptor, output, offset); (void)close(descriptor); return status;
}
static void platform_signal_handler(int number) { if (number == SIGTERM || number == SIGINT) platform_signal_request = LS200_SIGNAL_STOP; else if (number == SIGHUP) platform_signal_request = LS200_SIGNAL_RELOAD; }
ls200_status ls200_platform_install_signal_handlers(void) {
  struct sigaction action; (void)memset(&action, 0, sizeof(action)); action.sa_handler = platform_signal_handler;
  if (sigemptyset(&action.sa_mask) != 0) return LS200_STATUS_IO_ERROR;
  return (sigaction(SIGTERM, &action, NULL) == 0 && sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGHUP, &action, NULL) == 0) ? LS200_STATUS_OK : LS200_STATUS_IO_ERROR;
}
ls200_signal_request ls200_platform_take_signal_request(void) { sig_atomic_t request = platform_signal_request; platform_signal_request = LS200_SIGNAL_NONE; return request == LS200_SIGNAL_STOP ? LS200_SIGNAL_STOP : request == LS200_SIGNAL_RELOAD ? LS200_SIGNAL_RELOAD : LS200_SIGNAL_NONE; }
ls200_status ls200_platform_get_child_identity(int pid, ls200_child_process *out_child) {
  ls200_status status;
  if (pid <= 0 || out_child == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return errno == ESRCH ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  (void)memset(out_child, 0, sizeof(*out_child)); out_child->pid = pid;
  status = process_effective_identity((pid_t)pid, &out_child->uid, &out_child->gid);
  return status == LS200_STATUS_OK ? ls200_platform_process_birth_token((pid_t)pid, &out_child->start_time_ns) : status;
}
ls200_status ls200_platform_validate_child_identity(const ls200_child_process *child) {
  ls200_child_process current; ls200_status status;
  if (child == NULL || child->pid <= 0 || child->start_time_ns == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  status = ls200_platform_get_child_identity(child->pid, &current);
  if (status != LS200_STATUS_OK) return status;
  return current.uid != child->uid || current.gid != child->gid || current.start_time_ns != child->start_time_ns ? LS200_STATUS_SECURITY_ERROR : LS200_STATUS_OK;
}
ls200_status ls200_platform_reap_child(ls200_child_process *child, int *out_exit_status) {
  int status; pid_t result;
  if (child == NULL || out_exit_status == NULL || child->pid <= 0) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_validate_child_identity(child) != LS200_STATUS_OK) return LS200_STATUS_SECURITY_ERROR;
  do { result = waitpid((pid_t)child->pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
  if (result == 0) return LS200_STATUS_AGAIN; if (result < 0) return errno == ECHILD ? LS200_STATUS_END : LS200_STATUS_IO_ERROR;
  *out_exit_status = status; child->pid = -1; return LS200_STATUS_OK;
}
static ls200_status wait_for_child_stop(ls200_child_process *child, ls200_deadline deadline, int *out_status) {
  uint64_t now;
  for (;;) { pid_t result = waitpid((pid_t)child->pid, out_status, WNOHANG); if (result == (pid_t)child->pid || (result < 0 && errno == ECHILD)) { child->pid = -1; return LS200_STATUS_OK; } if (result < 0 && errno != EINTR) return LS200_STATUS_IO_ERROR; if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) return LS200_STATUS_IO_ERROR; if (now >= deadline.monotonic_ns) return LS200_STATUS_AGAIN; { struct timespec pause_time = {0, 10000000L}; (void)nanosleep(&pause_time, NULL); } }
}
static ls200_status force_stop_child(ls200_child_process *child, int *out_status) {
  (void)kill((pid_t)child->pid, SIGKILL); while (waitpid((pid_t)child->pid, out_status, 0) < 0) if (errno != EINTR) { child->pid = -1; return LS200_STATUS_TIMEOUT; }
  child->pid = -1; return LS200_STATUS_TIMEOUT;
}
ls200_status ls200_platform_stop_child(ls200_child_process *child, ls200_deadline deadline) {
  int wait_status; ls200_status status;
  if (child == NULL || child->pid <= 0) return LS200_STATUS_INVALID_ARGUMENT;
  if (ls200_platform_validate_child_identity(child) != LS200_STATUS_OK) return LS200_STATUS_SECURITY_ERROR;
  if (kill((pid_t)child->pid, SIGTERM) != 0 && errno != ESRCH) return LS200_STATUS_IO_ERROR;
  status = wait_for_child_stop(child, deadline, &wait_status);
  return status == LS200_STATUS_AGAIN ? force_stop_child(child, &wait_status) : status;
}
