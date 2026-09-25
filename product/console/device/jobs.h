#ifndef LS200_DEVICE_JOBS_H
#define LS200_DEVICE_JOBS_H
#include <jansson.h>

#define LS200_DEVICE_JOB_LIMIT 32U
typedef struct {
  int directory;
  int lock;
  int healthy;
  json_t *document;
} ls200_device_jobs;

enum ls200_device_job_result {
  LS200_JOB_ERROR = 0, LS200_JOB_CREATED = 1,
  LS200_JOB_DUPLICATE = 2, LS200_JOB_CONFLICT = 3, LS200_JOB_FULL = 4
};

/* The caller supplies a trusted, owner-only directory descriptor. Open takes
 * an exclusive lifetime lock and reconciles running jobs to uncertain. */
int ls200_device_jobs_open(ls200_device_jobs *store, int directory);
void ls200_device_jobs_close(ls200_device_jobs *store);
/* Stable principal + key identify intent across sessions and restarts. The
 * fingerprint must bind canonical operation/arguments/revision, never secrets.
 * No dispatch is permitted until CREATED and a durable running transition. */
int ls200_device_jobs_accept(ls200_device_jobs *store, const char *principal,
    const char *key, const char *operation, const char *fingerprint,
    json_t **job);
/* Reconciliation may resolve uncertain to succeeded/failed, never running.
 * Cancellation is available only before dispatch. No journal entries expire. */
int ls200_device_jobs_transition(ls200_device_jobs *store, const char *id,
    const char *state);
json_t *ls200_device_jobs_list(const ls200_device_jobs *store);
#endif
