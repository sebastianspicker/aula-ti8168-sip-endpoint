#ifndef AULA_DEVICE_JOBS_H
#define AULA_DEVICE_JOBS_H
#include <jansson.h>

#define AULA_DEVICE_JOB_LIMIT 32U
typedef struct {
  int directory;
  int lock;
  int healthy;
  json_t *document;
} aula_device_jobs;

/* The caller supplies a trusted, owner-only directory descriptor. Open takes
 * an exclusive lifetime lock and reconciles running jobs to uncertain. */
int aula_device_jobs_open(aula_device_jobs *store, int directory);
void aula_device_jobs_close(aula_device_jobs *store);
json_t *aula_device_jobs_list(const aula_device_jobs *store);
#endif
