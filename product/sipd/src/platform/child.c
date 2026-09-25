#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "platform_private.h"
#include "ls200_sipd/platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#if defined(__GLIBC__)
typedef __rlimit_resource_t ls200_rlimit_resource_t;
#else
typedef int ls200_rlimit_resource_t;
#endif

static int path_is_safe(const char *path) {
  const unsigned char *cursor = (const unsigned char *)path;
  if (path == NULL || path[0] != '/') return 0;
  while (*cursor != '\0') {
    if (*cursor < 0x20U || *cursor == 0x7fU) return 0;
    ++cursor;
  }
  return strstr(path, "/../") == NULL && strcmp(path + strlen(path) - (strlen(path) >= 3U ? 3U : 0U), "/..") != 0;
}

static int child_argument_is_safe(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  while (*cursor != '\0') {
    if (*cursor < 0x20U || *cursor == 0x7fU || *cursor == (unsigned char)'!' || *cursor == (unsigned char)';' ||
        *cursor == (unsigned char)'|' || *cursor == (unsigned char)'&' || *cursor == (unsigned char)'$' ||
        *cursor == (unsigned char)'`') return 0;
    ++cursor;
  }
  return 1;
}

static int valid_vector(const char *const *vector, size_t maximum_entries) {
  size_t index;
  if (vector == NULL) return 0;
  for (index = 0U; index < maximum_entries; ++index) {
    if (vector[index] == NULL) return index != 0U;
    if (strlen(vector[index]) == 0U || strlen(vector[index]) > 4096U || !child_argument_is_safe(vector[index])) return 0;
  }
  return 0;
}

static int valid_environment(const char *const *environment) {
  static const char *const allowed_names[] = {"LANG=", "LC_ALL=", "TZ="};
  size_t index;
  if (environment == NULL || !valid_vector(environment, 8U)) return 0;
  for (index = 0U; environment[index] != NULL; ++index) {
    size_t allowed_index;
    int matched = 0;
    for (allowed_index = 0U; allowed_index < sizeof(allowed_names) / sizeof(allowed_names[0]); ++allowed_index) {
      size_t prefix_length = strlen(allowed_names[allowed_index]);
      if (strncmp(environment[index], allowed_names[allowed_index], prefix_length) == 0 && environment[index][prefix_length] != '\0') { matched = 1; break; }
    }
    if (!matched) return 0;
  }
  return 1;
}

static int open_verified_child_executable(const ls200_child_process_config *config) {
  struct stat details;
  int descriptor;
  if (config == NULL || !path_is_safe(config->executable_path)) return -1;
  descriptor = open(config->executable_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return -1;
  if (fstat(descriptor, &details) != 0 || !S_ISREG(details.st_mode) || details.st_nlink != 1 ||
      (details.st_mode & 0022U) != 0U || (details.st_mode & (S_ISUID | S_ISGID)) != 0U ||
      (details.st_mode & 0111U) == 0U ||
      (details.st_uid != 0U && details.st_uid != geteuid() &&
       (!config->require_expected_identity || details.st_uid != (uid_t)config->expected_uid))) {
    (void)close(descriptor);
    return -1;
  }
  return descriptor;
}

static int move_internal_fd_above_stdio(int descriptor) {
  int relocated;
  if (descriptor >= 3) return descriptor;
  relocated = fcntl(descriptor, F_DUPFD, 3);
  if (relocated < 0 || fcntl(relocated, F_SETFD, FD_CLOEXEC) != 0) {
    if (relocated >= 0) (void)close(relocated);
    (void)close(descriptor);
    return -1;
  }
  (void)close(descriptor);
  return relocated;
}

static int relocate_pipe_fds_above_stdio(int pipe_fds[2]) {
  int relocated[2] = {-1, -1};
  size_t index;
  if (pipe_fds[0] >= 3 && pipe_fds[1] >= 3) {
    return fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == 0;
  }
  for (index = 0U; index < 2U; ++index) {
    relocated[index] = fcntl(pipe_fds[index], F_DUPFD, 3);
    if (relocated[index] < 0 || fcntl(relocated[index], F_SETFD, FD_CLOEXEC) != 0) {
      if (relocated[index] >= 0) (void)close(relocated[index]);
      if (relocated[0] >= 0) (void)close(relocated[0]);
      return 0;
    }
  }
  (void)close(pipe_fds[0]);
  (void)close(pipe_fds[1]);
  pipe_fds[0] = relocated[0];
  pipe_fds[1] = relocated[1];
  return 1;
}

static int fd_is_allowlisted(const ls200_child_process_config *config, int descriptor,
                             int retained_error_fd, int retained_executable_fd) {
  size_t index;
  if (descriptor == retained_error_fd || descriptor == retained_executable_fd) return 1;
  for (index = 0U; config != NULL && index < config->fd_allowlist_count; ++index) {
    if (config->fd_allowlist[index] == descriptor) return 1;
  }
  return 0;
}

static int fd_allowlist_is_valid(const ls200_child_process_config *config) {
  size_t index;
  size_t prior;
  if (config == NULL || config->fd_allowlist_count > 64U ||
      (config->fd_allowlist_count != 0U && config->fd_allowlist == NULL)) return 0;
  for (index = 0U; index < config->fd_allowlist_count; ++index) {
    if (config->fd_allowlist[index] < 0 || fcntl(config->fd_allowlist[index], F_GETFD) < 0) return 0;
    for (prior = 0U; prior < index; ++prior) {
      if (config->fd_allowlist[prior] == config->fd_allowlist[index]) return 0;
    }
  }
  return 1;
}

#if defined(__linux__)
static int parse_fd_name(const char *name, int *out_descriptor) {
  char *end;
  long value;
  errno = 0;
  value = strtol(name, &end, 10);
  if (errno != 0 || end == name || *end != '\0' || value < 0L || value > (long)INT_MAX) { errno = EINVAL; return 0; }
  *out_descriptor = (int)value;
  return 1;
}
#endif
static int close_candidate_fd(const ls200_child_process_config *config, int descriptor,
                              int retained_error_fd, int retained_executable_fd) {
  return descriptor <= STDERR_FILENO ||
      fd_is_allowlisted(config, descriptor, retained_error_fd, retained_executable_fd) || close(descriptor) == 0 || errno == EBADF;
}
static int close_uninherited_fds(const ls200_child_process_config *config,
                                 int retained_error_fd, int retained_executable_fd) {
#if defined(__linux__)
  DIR *directory;
  struct dirent *entry;
  int directory_fd;
  int result = 1;
  directory = opendir("/proc/self/fd");
  if (directory == NULL) return 0;
  directory_fd = dirfd(directory);
  for (;;) {
    int descriptor;
    errno = 0;
    entry = readdir(directory);
    if (entry == NULL) {
      result = errno == 0;
      break;
    }
    if (entry->d_name[0] == '.') continue;
    if (!parse_fd_name(entry->d_name, &descriptor)) { result = 0; break; }
    if (descriptor != directory_fd && !close_candidate_fd(config, descriptor, retained_error_fd, retained_executable_fd)) {
      result = 0;
      break;
    }
  }
  if (closedir(directory) != 0) result = 0;
  return result;
#else
  struct rlimit maximum_limit;
  rlim_t maximum;
  int descriptor;
  if (getrlimit(RLIMIT_NOFILE, &maximum_limit) != 0) return 0;
  maximum = maximum_limit.rlim_cur;
  if (maximum == RLIM_INFINITY || maximum > (rlim_t)INT_MAX) {
    errno = EOVERFLOW;
    return 0;
  }
  for (descriptor = 3; descriptor < (int)maximum; ++descriptor) {
    if (!close_candidate_fd(config, descriptor, retained_error_fd, retained_executable_fd)) return 0;
  }
  return 1;
#endif
}

static int checked_rlimit_value(uint64_t value, rlim_t *out_value) {
  rlim_t converted;
  if (out_value == NULL || value == UINT64_MAX) return 0;
  converted = (rlim_t)value;
  if ((uint64_t)converted != value || converted == RLIM_INFINITY) return 0;
  *out_value = converted;
  return 1;
}

static int apply_child_limit(ls200_rlimit_resource_t resource, uint64_t requested) {
  struct rlimit limit;
  rlim_t value;
  if (!checked_rlimit_value(requested, &value) || getrlimit(resource, &limit) != 0) return 0;
  if (value > limit.rlim_max) {
    errno = EPERM;
    return 0;
  }
  limit.rlim_cur = value;
  limit.rlim_max = value;
  return setrlimit(resource, &limit) == 0;
}

static int restrict_child_resources(const ls200_child_process_limits *limits) {
  return apply_child_limit(RLIMIT_CPU, limits->cpu_seconds) &&
         apply_child_limit(RLIMIT_AS, limits->address_space_bytes) &&
         apply_child_limit(RLIMIT_FSIZE, limits->file_size_bytes) &&
         apply_child_limit(RLIMIT_NPROC, limits->process_count) &&
         apply_child_limit(RLIMIT_NOFILE, limits->open_files) &&
         apply_child_limit(RLIMIT_CORE, limits->core_bytes);
}

static int child_limits_are_valid(const ls200_child_process_limits *limits) {
  rlim_t discarded;
  if (limits == NULL || limits->cpu_seconds == 0U || limits->address_space_bytes == 0U ||
      limits->process_count == 0U || limits->open_files < 3U || limits->core_bytes != 0U) return 0;
  return checked_rlimit_value(limits->cpu_seconds, &discarded) &&
         checked_rlimit_value(limits->address_space_bytes, &discarded) &&
         checked_rlimit_value(limits->file_size_bytes, &discarded) &&
         checked_rlimit_value(limits->process_count, &discarded) &&
         checked_rlimit_value(limits->open_files, &discarded) &&
         checked_rlimit_value(limits->core_bytes, &discarded);
}

static int redirect_fd(int source, int target) {
  if (source == target) return 1;
  return dup2(source, target) >= 0;
}

static int child_input_fds_are_valid(const ls200_child_process_config *config) {
  const int descriptors[] = {config->stdin_fd, config->stdout_fd, config->stderr_fd};
  size_t index;
  for (index = 0U; index < sizeof(descriptors) / sizeof(descriptors[0]); ++index) {
    if (descriptors[index] < -1 ||
        (descriptors[index] >= 0 && fcntl(descriptors[index], F_GETFD) < 0) ||
        (descriptors[index] >= 0 && descriptors[index] <= STDERR_FILENO &&
         descriptors[index] != (int)index)) return 0;
  }
  return 1;
}

static int reset_child_signal_state(void) {
  struct sigaction action;
  sigset_t empty_mask;
  int signal_number;
  (void)memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  if (sigemptyset(&action.sa_mask) != 0 || sigemptyset(&empty_mask) != 0 ||
      sigprocmask(SIG_SETMASK, &empty_mask, NULL) != 0) return 0;
#if defined(NSIG)
  for (signal_number = 1; signal_number < NSIG; ++signal_number) {
    if (signal_number != SIGKILL && signal_number != SIGSTOP &&
        sigaction(signal_number, &action, NULL) != 0 && errno != EINVAL) return 0;
  }
#else
  (void)signal_number;
  if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
      sigaction(SIGHUP, &action, NULL) != 0 || sigaction(SIGPIPE, &action, NULL) != 0) return 0;
#endif
  return 1;
}

static int transition_child_identity(const ls200_child_process_config *config) {
  if (config->require_expected_identity == 0) return 1;
  if (geteuid() == 0U) {
    if (setgroups(0, NULL) != 0) return 0;
#if defined(__linux__)
    if (setresgid((gid_t)config->expected_gid, (gid_t)config->expected_gid,
                  (gid_t)config->expected_gid) != 0 ||
        setresuid((uid_t)config->expected_uid, (uid_t)config->expected_uid,
                  (uid_t)config->expected_uid) != 0) return 0;
#else
    if (setgid((gid_t)config->expected_gid) != 0 ||
        setuid((uid_t)config->expected_uid) != 0) return 0;
#endif
  }
  if (config->require_non_root_identity != 0 && getgroups(0, NULL) != 0)
    return 0;
  return getuid() == (uid_t)config->expected_uid && geteuid() == (uid_t)config->expected_uid &&
         getgid() == (gid_t)config->expected_gid && getegid() == (gid_t)config->expected_gid;
}

static int configured_identity_is_valid(const ls200_child_process_config *config) {
  return config != NULL && (uint32_t)(uid_t)config->expected_uid == config->expected_uid &&
         (uint32_t)(gid_t)config->expected_gid == config->expected_gid;
}

static int identity_transition_is_authorized(const ls200_child_process_config *config) {
  if (config->require_expected_identity == 0) return 1;
  if (geteuid() == 0U) return 1;
  if (config->expected_uid != (uint32_t)geteuid() ||
      config->expected_gid != (uint32_t)getegid()) return 0;
  return config->require_non_root_identity == 0 || getgroups(0, NULL) == 0;
}

static int required_non_root_identity_is_valid(const ls200_child_process_config *config) {
  return config->require_non_root_identity == 0 ||
         (config->require_expected_identity != 0 && config->expected_uid != 0U &&
          config->expected_gid != 0U);
}

static int apply_no_new_privs(const ls200_child_process_config *config) {
  if (config->require_no_new_privs == 0) return 1;
#if defined(__linux__) && defined(PR_SET_NO_NEW_PRIVS)
  return prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) == 0;
#else
  errno = ENOTSUP;
  return 0;
#endif
}

static void report_child_failure(int error_fd) {
  int saved_errno = errno == 0 ? EIO : errno;
  ssize_t written;
  do { written = write(error_fd, &saved_errno, sizeof(saved_errno)); } while (written < 0 && errno == EINTR);
}

static void execute_verified_child(int executable_fd, const ls200_child_process_config *config) {
#if defined(__linux__)
  (void)fexecve(executable_fd, (char *const *)config->argv, (char *const *)config->environment);
#else
  char descriptor_path[32];
  int written = snprintf(descriptor_path, sizeof(descriptor_path), "/dev/fd/%d", executable_fd);
  if (written > 0 && (size_t)written < sizeof(descriptor_path)) {
    (void)execve(descriptor_path, (char *const *)config->argv, (char *const *)config->environment);
  } else {
    errno = ENAMETOOLONG;
  }
#endif
}

static int child_policy_fields_are_valid(const ls200_child_process_config *config) {
  return config != NULL && config->executable_path != NULL &&
      config->require_expected_identity >= 0 && config->require_expected_identity <= 1 &&
      config->require_non_root_identity >= 0 && config->require_non_root_identity <= 1 &&
      config->close_unlisted_fds == 1 && config->require_no_new_privs >= 0 &&
      config->require_no_new_privs <= 1;
}
static int child_policy_dependencies_are_valid(const ls200_child_process_config *config) {
  return valid_vector(config->argv, 64U) && valid_environment(config->environment) &&
      strcmp(config->argv[0], config->executable_path) == 0 &&
      child_input_fds_are_valid(config) && fd_allowlist_is_valid(config) &&
      child_limits_are_valid(&config->limits) &&
      required_non_root_identity_is_valid(config) &&
      (config->require_expected_identity == 0 || configured_identity_is_valid(config));
}
static int child_allowlist_fits_limits(const ls200_child_process_config *config) {
  size_t index;
  for (index = 0U; index < config->fd_allowlist_count; ++index) {
    if ((uint64_t)config->fd_allowlist[index] >= config->limits.open_files) return 0;
  }
  return 1;
}
static int child_policy_is_valid(const ls200_child_process_config *config) {
  return child_policy_fields_are_valid(config) && child_policy_dependencies_are_valid(config) && child_allowlist_fits_limits(config);
}

typedef struct child_spawn_resources {
  int executable_fd;
  int null_fd;
  int error_pipe[2];
} child_spawn_resources;

static void close_child_spawn_resources(child_spawn_resources *resources) {
  if (resources->executable_fd >= 0) (void)close(resources->executable_fd);
  if (resources->null_fd >= 0) (void)close(resources->null_fd);
  if (resources->error_pipe[0] >= 0) (void)close(resources->error_pipe[0]);
  if (resources->error_pipe[1] >= 0) (void)close(resources->error_pipe[1]);
}

static ls200_status prepare_child_spawn_resources(const ls200_child_process_config *config,
                                                  child_spawn_resources *resources) {
  resources->executable_fd = open_verified_child_executable(config);
  if (resources->executable_fd < 0) return LS200_STATUS_SECURITY_ERROR;
  resources->executable_fd = move_internal_fd_above_stdio(resources->executable_fd);
  if (resources->executable_fd < 0) return LS200_STATUS_IO_ERROR;
  resources->null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (resources->null_fd < 0) return LS200_STATUS_IO_ERROR;
  resources->null_fd = move_internal_fd_above_stdio(resources->null_fd);
  if (resources->null_fd < 0) return LS200_STATUS_IO_ERROR;
  if (pipe(resources->error_pipe) != 0 || !relocate_pipe_fds_above_stdio(resources->error_pipe)) return LS200_STATUS_IO_ERROR;
  return LS200_STATUS_OK;
}

static void run_child_process(const ls200_child_process_config *config,
                              const child_spawn_resources *resources) {
  (void)close(resources->error_pipe[0]);
  (void)umask(0077U);
  if (chdir("/") != 0 || !reset_child_signal_state() || !transition_child_identity(config)) { report_child_failure(resources->error_pipe[1]); _exit(127); }
  if (!redirect_fd(config->stdin_fd < 0 ? resources->null_fd : config->stdin_fd, STDIN_FILENO) || !redirect_fd(config->stdout_fd < 0 ? resources->null_fd : config->stdout_fd, STDOUT_FILENO) || !redirect_fd(config->stderr_fd < 0 ? resources->null_fd : config->stderr_fd, STDERR_FILENO)) { report_child_failure(resources->error_pipe[1]); _exit(127); }
  if (!close_uninherited_fds(config, resources->error_pipe[1], resources->executable_fd) || !restrict_child_resources(&config->limits) || !apply_no_new_privs(config)) { report_child_failure(resources->error_pipe[1]); _exit(127); }
  execute_verified_child(resources->executable_fd, config);
  report_child_failure(resources->error_pipe[1]);
  _exit(127);
}

static void discard_child_process(pid_t child_pid, ls200_child_process *child) {
  (void)kill(child_pid, SIGKILL);
  (void)waitpid(child_pid, NULL, 0);
  (void)memset(child, 0, sizeof(*child));
}

static ls200_status complete_child_spawn(const ls200_child_process_config *config,
                                         pid_t child_pid, ls200_child_process *out_child) {
  out_child->pid = (int)child_pid;
  out_child->uid = config->require_expected_identity != 0 ? config->expected_uid : (uint32_t)geteuid();
  out_child->gid = config->require_expected_identity != 0 ? config->expected_gid : (uint32_t)getegid();
  if (ls200_platform_process_birth_token(child_pid, &out_child->start_time_ns) != LS200_STATUS_OK) { discard_child_process(child_pid, out_child); return LS200_STATUS_IO_ERROR; }
  if (ls200_platform_validate_child_identity(out_child) != LS200_STATUS_OK) { discard_child_process(child_pid, out_child); return LS200_STATUS_SECURITY_ERROR; }
  return LS200_STATUS_OK;
}

ls200_status ls200_platform_spawn_child(const ls200_child_process_config *config,
                                        ls200_child_process *out_child) {
  child_spawn_resources resources = {-1, -1, {-1, -1}};
  pid_t child_pid;
  int child_error = 0;
  ssize_t received;
  ls200_status status;
  if (out_child == NULL || out_child->pid > 0 || !child_policy_is_valid(config)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (!identity_transition_is_authorized(config)) return LS200_STATUS_PERMISSION_DENIED;
#if defined(__APPLE__)
  /* macOS has no fexecve(), and executing /dev/fd/N is denied. Falling back
   * to the pathname after verification would reintroduce a rename race. */
  return LS200_STATUS_UNSUPPORTED;
#endif
  status = prepare_child_spawn_resources(config, &resources);
  if (status != LS200_STATUS_OK) { close_child_spawn_resources(&resources); return status; }
  child_pid = fork();
  if (child_pid < 0) {
    close_child_spawn_resources(&resources);
    return LS200_STATUS_IO_ERROR;
  }
  if (child_pid == 0) run_child_process(config, &resources);
  (void)close(resources.executable_fd); resources.executable_fd = -1;
  (void)close(resources.null_fd); resources.null_fd = -1;
  (void)close(resources.error_pipe[1]); resources.error_pipe[1] = -1;
  do { received = read(resources.error_pipe[0], &child_error, sizeof(child_error)); } while (received < 0 && errno == EINTR);
  (void)close(resources.error_pipe[0]); resources.error_pipe[0] = -1;
  (void)child_error;
  if (received > 0) { (void)waitpid(child_pid, NULL, 0); return LS200_STATUS_IO_ERROR; }
  if (received < 0) { (void)kill(child_pid, SIGKILL); (void)waitpid(child_pid, NULL, 0); return LS200_STATUS_IO_ERROR; }
  return complete_child_spawn(config, child_pid, out_child);
}
