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

enum aula_device_job_result {
  AULA_JOB_ERROR = 0, AULA_JOB_CREATED = 1,
  AULA_JOB_DUPLICATE = 2, AULA_JOB_CONFLICT = 3, AULA_JOB_FULL = 4
};

/* The caller supplies a trusted, owner-only directory descriptor. Open takes
 * an exclusive lifetime lock and reconciles running jobs to uncertain. */
int aula_device_jobs_open(aula_device_jobs *store, int directory);
void aula_device_jobs_close(aula_device_jobs *store);
/* Stable principal + key identify intent across sessions and restarts. The
 * fingerprint must bind canonical operation/arguments/revision, never secrets.
 * No dispatch is permitted until CREATED and a durable running transition. */
int aula_device_jobs_accept(aula_device_jobs *store, const char *principal,
    const char *key, const char *operation, const char *fingerprint,
    json_t **job);
/* Reconciliation may resolve uncertain to succeeded/failed, never running.
 * Cancellation is available only before dispatch. No journal entries expire. */
int aula_device_jobs_transition(aula_device_jobs *store, const char *id,
    const char *state);
json_t *aula_device_jobs_list(const aula_device_jobs *store);
#endif
