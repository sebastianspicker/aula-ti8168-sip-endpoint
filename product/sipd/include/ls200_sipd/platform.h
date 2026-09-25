#ifndef LS200_SIPD_PLATFORM_H
#define LS200_SIPD_PLATFORM_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_signal_request {
  LS200_SIGNAL_NONE = 0,
  LS200_SIGNAL_STOP = 1,
  LS200_SIGNAL_RELOAD = 2
} ls200_signal_request;

/* Every limit is an enforced hard and soft cap.  core_bytes must be zero. */
typedef struct ls200_child_process_limits {
  uint64_t cpu_seconds;
  uint64_t address_space_bytes;
  uint64_t file_size_bytes;
  uint64_t process_count;
  uint64_t open_files;
  uint64_t core_bytes;
} ls200_child_process_limits;

typedef struct ls200_child_process_config {
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
  ls200_child_process_limits limits;
  /* When set, fail rather than launch without Linux no_new_privs support. */
  int require_no_new_privs;
} ls200_child_process_config;

typedef struct ls200_child_process {
  int pid;
  uint64_t start_time_ns;
  uint32_t uid;
  uint32_t gid;
} ls200_child_process;

ls200_status ls200_platform_monotonic_now(uint64_t *out_ns);
/* RFC 3550 NTP timestamp from CLOCK_REALTIME; not for deadline calculations. */
ls200_status ls200_platform_ntp_now(uint64_t *out_ntp);
ls200_status ls200_platform_random_bytes(ls200_mutable_bytes *output);
ls200_status ls200_platform_install_signal_handlers(void);
ls200_signal_request ls200_platform_take_signal_request(void);
/*
 * Executes a verified executable inode with an explicit argv; it never invokes
 * a shell.  The child starts in / with umask 0077, default signal dispositions,
 * an empty signal mask, only standard and allowlisted descriptors, and the
 * configured hard resource caps.  Set require_expected_identity to drop to
 * expected_uid/expected_gid when the parent is authorized to do so.  Set
 * require_non_root_identity for a privileged parent to require an irreversible
 * drop to a non-root UID and GID before exec.
 */
ls200_status ls200_platform_spawn_child(const ls200_child_process_config *config,
                                        ls200_child_process *out_child);
ls200_status ls200_platform_stop_child(ls200_child_process *child,
                                       ls200_deadline deadline);
ls200_status ls200_platform_get_child_identity(int pid,
                                               ls200_child_process *out_child);
ls200_status ls200_platform_validate_child_identity(const ls200_child_process *child);
ls200_status ls200_platform_reap_child(ls200_child_process *child, int *out_exit_status);

#ifdef __cplusplus
}
#endif

#endif
