#ifndef LS200_RECEIVE_MONITOR_WORKER_H
#define LS200_RECEIVE_MONITOR_WORKER_H

#include "ls200_sipd/platform.h"

typedef struct ls200_monitor_worker {
  ls200_child_process child;
  int input_fd;
  uint8_t *pending;
  size_t capacity;
  size_t offset;
  size_t length;
} ls200_monitor_worker;

ls200_status ls200_monitor_worker_start(ls200_monitor_worker *worker,
                                         const char *path, int video);
ls200_status ls200_monitor_worker_flush(ls200_monitor_worker *worker);
ls200_status ls200_monitor_worker_submit(ls200_monitor_worker *worker, ls200_bytes data);
void ls200_monitor_worker_stop(ls200_monitor_worker *worker);

#endif
