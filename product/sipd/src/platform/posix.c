#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "platform_private.h"
#include "aula_sipd/platform.h"

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
#if defined(AULA_SIPD_HAVE_GETRANDOM) && AULA_SIPD_HAVE_GETRANDOM && !defined(AULA_SIPD_TARGET_ARM_EABI5)
#include <sys/random.h>
#endif

static volatile sig_atomic_t platform_signal_request = AULA_SIGNAL_NONE;

#if defined(__linux__)
static aula_status parse_linux_birth_token(char *buffer, uint64_t *out_token) {
  char *cursor = strrchr(buffer, ')');
  char *end;
  unsigned int field = 3U;
  if (cursor == NULL || cursor[1] != ' ') return AULA_STATUS_INVALID_DATA;
  cursor += 2;
  while (*cursor != '\0') {
    char *token = cursor;
    while (*cursor != '\0' && *cursor != ' ') ++cursor;
    if (*cursor == ' ') { *cursor = '\0'; ++cursor; while (*cursor == ' ') ++cursor; }
    if (field == 22U) { unsigned long long value; errno = 0; value = strtoull(token, &end, 10); if (errno != 0 || end == token || *end != '\0' || value == 0ULL) return AULA_STATUS_INVALID_DATA; *out_token = (uint64_t)value; return AULA_STATUS_OK; }
    ++field;
  }
  return AULA_STATUS_INVALID_DATA;
}
static aula_status linux_process_birth_token(pid_t pid, uint64_t *out_token) {
  char path[64]; char buffer[1024]; ssize_t count; int descriptor;
  int written = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return AULA_STATUS_LIMIT_EXCEEDED;
  descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return errno == ENOENT ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  do { count = read(descriptor, buffer, sizeof(buffer) - 1U); } while (count < 0 && errno == EINTR);
  (void)close(descriptor);
  if (count <= 0 || (size_t)count >= sizeof(buffer)) return AULA_STATUS_IO_ERROR;
  buffer[count] = '\0'; return parse_linux_birth_token(buffer, out_token);
}
#endif
#if defined(__APPLE__)
static aula_status macos_process_birth_token(pid_t pid, uint64_t *out_token) {
  struct proc_bsdinfo info;
  int bytes = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &info, (int)sizeof(info));
  if (bytes != (int)sizeof(info)) return errno == ESRCH ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  if (info.pbi_start_tvsec == 0U && info.pbi_start_tvusec == 0U) return AULA_STATUS_INVALID_DATA;
  *out_token = ((uint64_t)info.pbi_start_tvsec * UINT64_C(1000000)) + (uint64_t)info.pbi_start_tvusec;
  return AULA_STATUS_OK;
}
#endif
aula_status aula_platform_process_birth_token(pid_t pid, uint64_t *out_token) {
  if (pid <= 0 || out_token == NULL) return AULA_STATUS_INVALID_ARGUMENT;
#if defined(__linux__)
  return linux_process_birth_token(pid, out_token);
#elif defined(__APPLE__)
  return macos_process_birth_token(pid, out_token);
#else
  return AULA_STATUS_UNSUPPORTED;
#endif
}

static aula_status process_effective_identity(pid_t pid, uint32_t *out_uid, uint32_t *out_gid) {
  if (pid <= 0 || out_uid == NULL || out_gid == NULL) return AULA_STATUS_INVALID_ARGUMENT;
#if defined(__linux__)
  char path[64]; struct stat details; int written = snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
  if (written <= 0 || (size_t)written >= sizeof(path)) return AULA_STATUS_LIMIT_EXCEEDED;
  if (stat(path, &details) != 0) return errno == ENOENT ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  if ((uint64_t)details.st_uid > UINT32_MAX || (uint64_t)details.st_gid > UINT32_MAX) return AULA_STATUS_LIMIT_EXCEEDED;
  *out_uid = (uint32_t)details.st_uid; *out_gid = (uint32_t)details.st_gid; return AULA_STATUS_OK;
#elif defined(__APPLE__)
  struct proc_bsdinfo info; int bytes = proc_pidinfo((int)pid, PROC_PIDTBSDINFO, 0, &info, (int)sizeof(info));
  if (bytes != (int)sizeof(info)) return errno == ESRCH ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  *out_uid = (uint32_t)info.pbi_uid; *out_gid = (uint32_t)info.pbi_gid; return AULA_STATUS_OK;
#else
  (void)pid; return AULA_STATUS_UNSUPPORTED;
#endif
}

aula_status aula_platform_monotonic_now(uint64_t *out_ns) {
  struct timespec now;
  if (out_ns == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0L || now.tv_nsec < 0L) return AULA_STATUS_IO_ERROR;
  *out_ns = ((uint64_t)now.tv_sec * UINT64_C(1000000000)) + (uint64_t)now.tv_nsec; return AULA_STATUS_OK;
}
aula_status aula_platform_ntp_now(uint64_t *out_ntp) {
  const uint64_t epoch = UINT64_C(2208988800); struct timespec now; uint64_t seconds; uint64_t fraction;
  if (out_ntp == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0L || now.tv_nsec < 0L || now.tv_nsec >= 1000000000L) return AULA_STATUS_IO_ERROR;
  if ((uint64_t)now.tv_sec > UINT64_MAX - epoch) return AULA_STATUS_LIMIT_EXCEEDED;
  seconds = (uint64_t)now.tv_sec + epoch; fraction = ((uint64_t)now.tv_nsec << 32U) / UINT64_C(1000000000);
  if (fraction > UINT32_MAX) return AULA_STATUS_INTERNAL_ERROR;
  *out_ntp = ((seconds & UINT64_C(0xffffffff)) << 32U) | fraction; return AULA_STATUS_OK;
}
static aula_status fill_random_descriptor(int descriptor, aula_mutable_bytes *output, size_t offset) {
  while (offset < output->capacity) { ssize_t count = read(descriptor, output->data + offset, output->capacity - offset); if (count > 0) { offset += (size_t)count; continue; } if (count < 0 && errno == EINTR) continue; return AULA_STATUS_IO_ERROR; }
  output->length = offset; return AULA_STATUS_OK;
}
static aula_status fill_random_getrandom(aula_mutable_bytes *output, size_t *out_offset) {
  size_t offset = 0U;
#if !defined(AULA_SIPD_HAVE_GETRANDOM) || !AULA_SIPD_HAVE_GETRANDOM || defined(AULA_SIPD_TARGET_ARM_EABI5)
  (void)output;
#endif
#if defined(AULA_SIPD_HAVE_GETRANDOM) && AULA_SIPD_HAVE_GETRANDOM && !defined(AULA_SIPD_TARGET_ARM_EABI5)
  while (offset < output->capacity) { ssize_t count = getrandom(output->data + offset, output->capacity - offset, 0); if (count > 0) { offset += (size_t)count; continue; } if (count < 0 && errno == EINTR) continue; if (count < 0 && (errno == ENOSYS || errno == EAGAIN)) break; return AULA_STATUS_IO_ERROR; }
#endif
  *out_offset = offset;
  return AULA_STATUS_OK;
}
aula_status aula_platform_random_bytes(aula_mutable_bytes *output) {
  int descriptor; size_t offset; aula_status status;
  if (output == NULL || output->data == NULL || output->capacity == 0U || output->length != 0U) return AULA_STATUS_INVALID_ARGUMENT;
  status = fill_random_getrandom(output, &offset);
  if (status != AULA_STATUS_OK) return status;
  if (offset == output->capacity) { output->length = offset; return AULA_STATUS_OK; }
  descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if (descriptor < 0) return AULA_STATUS_IO_ERROR;
  status = fill_random_descriptor(descriptor, output, offset); (void)close(descriptor); return status;
}
static void platform_signal_handler(int number) { if (number == SIGTERM || number == SIGINT) platform_signal_request = AULA_SIGNAL_STOP; else if (number == SIGHUP) platform_signal_request = AULA_SIGNAL_RELOAD; }
aula_status aula_platform_install_signal_handlers(void) {
  struct sigaction action; (void)memset(&action, 0, sizeof(action)); action.sa_handler = platform_signal_handler;
  if (sigemptyset(&action.sa_mask) != 0) return AULA_STATUS_IO_ERROR;
  return (sigaction(SIGTERM, &action, NULL) == 0 && sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGHUP, &action, NULL) == 0) ? AULA_STATUS_OK : AULA_STATUS_IO_ERROR;
}
aula_signal_request aula_platform_take_signal_request(void) { sig_atomic_t request = platform_signal_request; platform_signal_request = AULA_SIGNAL_NONE; return request == AULA_SIGNAL_STOP ? AULA_SIGNAL_STOP : request == AULA_SIGNAL_RELOAD ? AULA_SIGNAL_RELOAD : AULA_SIGNAL_NONE; }
aula_status aula_platform_get_child_identity(int pid, aula_child_process *out_child) {
  aula_status status;
  if (pid <= 0 || out_child == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (kill((pid_t)pid, 0) != 0 && errno != EPERM) return errno == ESRCH ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  (void)memset(out_child, 0, sizeof(*out_child)); out_child->pid = pid;
  status = process_effective_identity((pid_t)pid, &out_child->uid, &out_child->gid);
  return status == AULA_STATUS_OK ? aula_platform_process_birth_token((pid_t)pid, &out_child->start_time_ns) : status;
}
aula_status aula_platform_validate_child_identity(const aula_child_process *child) {
  aula_child_process current; aula_status status;
  if (child == NULL || child->pid <= 0 || child->start_time_ns == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_platform_get_child_identity(child->pid, &current);
  if (status != AULA_STATUS_OK) return status;
  return current.uid != child->uid || current.gid != child->gid || current.start_time_ns != child->start_time_ns ? AULA_STATUS_SECURITY_ERROR : AULA_STATUS_OK;
}
aula_status aula_platform_reap_child(aula_child_process *child, int *out_exit_status) {
  int status; pid_t result;
  if (child == NULL || out_exit_status == NULL || child->pid <= 0) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_platform_validate_child_identity(child) != AULA_STATUS_OK) return AULA_STATUS_SECURITY_ERROR;
  do { result = waitpid((pid_t)child->pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
  if (result == 0) return AULA_STATUS_AGAIN; if (result < 0) return errno == ECHILD ? AULA_STATUS_END : AULA_STATUS_IO_ERROR;
  *out_exit_status = status; child->pid = -1; return AULA_STATUS_OK;
}
static aula_status wait_for_child_stop(aula_child_process *child, aula_deadline deadline, int *out_status) {
  uint64_t now;
  for (;;) { pid_t result = waitpid((pid_t)child->pid, out_status, WNOHANG); if (result == (pid_t)child->pid || (result < 0 && errno == ECHILD)) { child->pid = -1; return AULA_STATUS_OK; } if (result < 0 && errno != EINTR) return AULA_STATUS_IO_ERROR; if (aula_platform_monotonic_now(&now) != AULA_STATUS_OK) return AULA_STATUS_IO_ERROR; if (now >= deadline.monotonic_ns) return AULA_STATUS_AGAIN; { struct timespec pause_time = {0, 10000000L}; (void)nanosleep(&pause_time, NULL); } }
}
static aula_status force_stop_child(aula_child_process *child, int *out_status) {
  (void)kill((pid_t)child->pid, SIGKILL); while (waitpid((pid_t)child->pid, out_status, 0) < 0) if (errno != EINTR) { child->pid = -1; return AULA_STATUS_TIMEOUT; }
  child->pid = -1; return AULA_STATUS_TIMEOUT;
}
aula_status aula_platform_stop_child(aula_child_process *child, aula_deadline deadline) {
  int wait_status; aula_status status;
  if (child == NULL || child->pid <= 0) return AULA_STATUS_INVALID_ARGUMENT;
  if (aula_platform_validate_child_identity(child) != AULA_STATUS_OK) return AULA_STATUS_SECURITY_ERROR;
  if (kill((pid_t)child->pid, SIGTERM) != 0 && errno != ESRCH) return AULA_STATUS_IO_ERROR;
  status = wait_for_child_stop(child, deadline, &wait_status);
  return status == AULA_STATUS_AGAIN ? force_stop_child(child, &wait_status) : status;
}
