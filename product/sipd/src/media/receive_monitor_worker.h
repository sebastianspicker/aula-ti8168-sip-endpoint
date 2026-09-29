#ifndef AULA_RECEIVE_MONITOR_WORKER_H
#define AULA_RECEIVE_MONITOR_WORKER_H

#include "aula_sipd/platform.h"

typedef struct aula_monitor_worker {
  aula_child_process child;
  int input_fd;
  uint8_t *pending;
  size_t capacity;
  size_t offset;
  size_t length;
} aula_monitor_worker;

aula_status aula_monitor_worker_start(aula_monitor_worker *worker,
                                         const char *path, int video);
aula_status aula_monitor_worker_flush(aula_monitor_worker *worker);
aula_status aula_monitor_worker_submit(aula_monitor_worker *worker, aula_bytes data);
void aula_monitor_worker_stop(aula_monitor_worker *worker);

#endif
