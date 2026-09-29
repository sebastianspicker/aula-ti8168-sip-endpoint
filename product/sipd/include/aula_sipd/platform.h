#ifndef AULA_SIPD_PLATFORM_H
#define AULA_SIPD_PLATFORM_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_signal_request {
  AULA_SIGNAL_NONE = 0,
  AULA_SIGNAL_STOP = 1,
  AULA_SIGNAL_RELOAD = 2
} aula_signal_request;

/* Every limit is an enforced hard and soft cap.  core_bytes must be zero. */
typedef struct aula_child_process_limits {
  uint64_t cpu_seconds;
  uint64_t address_space_bytes;
  uint64_t file_size_bytes;
  uint64_t process_count;
  uint64_t open_files;
  uint64_t core_bytes;
} aula_child_process_limits;

typedef struct aula_child_process_config {
  const char *executable_path;
  const char *const *argv;
  const char *const *environment;
  int stdin_fd;
  int stdout_fd;
  int stderr_fd;
  const int *fd_allowlist;
  size_t fd_allowlist_count;
  uint32_t expected_uid;
  uint32_t expected_gid;
  /* A privileged parent clears supplementary groups, then sets these IDs. */
  int require_expected_identity;
  /* Required for privileged media children: reject UID or GID zero before fork. */
  int require_non_root_identity;
  /* Required: must be one so every nonstandard descriptor is closed. */
  int close_unlisted_fds;
  /* Required hard caps; core_bytes must remain zero. */
  aula_child_process_limits limits;
  /* When set, fail rather than launch without Linux no_new_privs support. */
  int require_no_new_privs;
} aula_child_process_config;

typedef struct aula_child_process {
  int pid;
  uint64_t start_time_ns;
  uint32_t uid;
  uint32_t gid;
} aula_child_process;

aula_status aula_platform_monotonic_now(uint64_t *out_ns);
/* RFC 3550 NTP timestamp from CLOCK_REALTIME; not for deadline calculations. */
aula_status aula_platform_ntp_now(uint64_t *out_ntp);
aula_status aula_platform_random_bytes(aula_mutable_bytes *output);
aula_status aula_platform_install_signal_handlers(void);
aula_signal_request aula_platform_take_signal_request(void);
/*
 * Executes a verified executable inode with an explicit argv; it never invokes
 * a shell.  The child starts in / with umask 0077, default signal dispositions,
 * an empty signal mask, only standard and allowlisted descriptors, and the
 * configured hard resource caps.  Set require_expected_identity to drop to
 * expected_uid/expected_gid when the parent is authorized to do so.  Set
 * require_non_root_identity for a privileged parent to require an irreversible
 * drop to a non-root UID and GID before exec.
 */
aula_status aula_platform_spawn_child(const aula_child_process_config *config,
                                        aula_child_process *out_child);
aula_status aula_platform_stop_child(aula_child_process *child,
                                       aula_deadline deadline);
aula_status aula_platform_get_child_identity(int pid,
                                               aula_child_process *out_child);
aula_status aula_platform_validate_child_identity(const aula_child_process *child);
aula_status aula_platform_reap_child(aula_child_process *child, int *out_exit_status);

#ifdef __cplusplus
}
#endif

#endif
