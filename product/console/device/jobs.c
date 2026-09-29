#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "jobs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define JOURNAL_LIMIT 32768U

static int owned_file(int fd) {
  struct stat info;
  return fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_uid == geteuid() && (info.st_mode & 0077) == 0 &&
      info.st_nlink == 1 && info.st_size >= 0 && info.st_size <= (off_t)JOURNAL_LIMIT;
}

static int job_token(const char *text, size_t maximum) {
  size_t length;
  if (text == NULL || (length = strlen(text)) == 0 || length > maximum) return 0;
  return strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@") == length;
}

static int job_state(const char *state) {
  static const char *const states[] = {
    "queued", "running", "succeeded", "failed", "cancelled", "uncertain"
  };
  if (state == NULL) return 0;
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
    if (strcmp(state, states[i]) == 0) return 1;
  return 0;
}

static int fingerprint_valid(const char *value) {
  return value != NULL && strlen(value) == 64 &&
      strspn(value, "0123456789abcdef") == 64;
}

static const char *job_text(const json_t *job, const char *key) {
  return json_string_value(json_object_get(job, key));
}

static int job_valid(json_t *job, size_t index) {
  char expected[24];
  snprintf(expected, sizeof(expected), "job-%08u", (unsigned)index + 1);
  if (!json_is_object(job) || json_object_size(job) != 6 ||
      !job_token(job_text(job, "id"), 20)) return 0;
  return strcmp(job_text(job, "id"), expected) == 0 &&
      job_token(job_text(job, "principal"), 64) &&
      job_token(job_text(job, "key"), 64) &&
      job_token(job_text(job, "operation"), 48) &&
      fingerprint_valid(job_text(job, "fingerprint")) &&
      job_state(job_text(job, "state"));
}

static int duplicate_intent(json_t *jobs, size_t index, json_t *job) {
  for (size_t i = 0; i < index; ++i) {
    json_t *previous = json_array_get(jobs, i);
    if (strcmp(job_text(previous, "principal"), job_text(job, "principal")) == 0 &&
        strcmp(job_text(previous, "key"), job_text(job, "key")) == 0) return 1;
  }
  return 0;
}

static int journal_valid(json_t *document) {
  json_t *jobs = json_object_get(document, "jobs"), *job;
  size_t index;
  if (!json_is_object(document) || json_object_size(document) != 2 ||
      !json_is_integer(json_object_get(document, "version")) ||
      json_integer_value(json_object_get(document, "version")) != 1 ||
      !json_is_array(jobs) || json_array_size(jobs) > AULA_DEVICE_JOB_LIMIT) return 0;
  json_array_foreach(jobs, index, job)
    if (!job_valid(job, index) || duplicate_intent(jobs, index, job)) return 0;
  return 1;
}

static int journal_write(aula_device_jobs *store) {
  char *encoded = json_dumps(store->document, JSON_COMPACT | JSON_SORT_KEYS);
  size_t offset = 0, length = encoded == NULL ? 0 : strlen(encoded);
  int fd, valid = 0;
  store->healthy = 0;
  if (length == 0 || length > JOURNAL_LIMIT) { free(encoded); return 0; }
  fd = openat(store->directory, "jobs.pending", O_WRONLY | O_CREAT | O_EXCL |
      O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) { free(encoded); return 0; }
  while (offset < length) {
    ssize_t count = write(fd, encoded + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) goto done;
    offset += (size_t)count;
  }
  valid = fsync(fd) == 0;
done:
  if (close(fd) != 0) valid = 0;
  free(encoded);
  if (valid) valid = renameat(store->directory, "jobs.pending",
      store->directory, "jobs.json") == 0 && fsync(store->directory) == 0;
  /* Any failure poisons this handle: a rename may have committed. Do not
   * dispatch or retry against uncertain in-memory state. Restart to reconcile. */
  store->healthy = valid;
  return valid;
}

static int discard_pending(aula_device_jobs *store) {
  int fd = openat(store->directory, "jobs.pending", O_RDONLY | O_NOFOLLOW |
      O_NONBLOCK | O_CLOEXEC);
  int valid;
  if (fd < 0) return errno == ENOENT;
  valid = owned_file(fd);
  close(fd);
  return valid && unlinkat(store->directory, "jobs.pending", 0) == 0 &&
      fsync(store->directory) == 0;
}

static int load_journal(aula_device_jobs *store) {
  json_error_t error;
  int fd = openat(store->directory, "jobs.json", O_RDONLY | O_NOFOLLOW |
      O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno != ENOENT) return 0;
    store->document = json_pack("{s:i,s:[]}", "version", 1, "jobs");
    return store->document != NULL && journal_write(store);
  }
  if (owned_file(fd)) store->document = json_loadfd(fd, JSON_REJECT_DUPLICATES, &error);
  close(fd);
  return journal_valid(store->document);
}

static int recover_journal(aula_device_jobs *store) {
  json_t *job, *jobs = json_object_get(store->document, "jobs");
  size_t index;
  int changed = 0;
  json_array_foreach(jobs, index, job) {
    if (strcmp(job_text(job, "state"), "running") != 0) continue;
    if (json_object_set_new(job, "state", json_string("uncertain")) != 0) return 0;
    changed = 1;
  }
  return !changed || journal_write(store);
}

int aula_device_jobs_open(aula_device_jobs *store, int directory) {
  struct stat info;
  if (store == NULL) return 0;
  *store = (aula_device_jobs){-1, -1, 0, NULL};
  if (fstat(directory, &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077) != 0) return 0;
  store->directory = fcntl(directory, F_DUPFD_CLOEXEC, 0);
  if (store->directory < 0) return 0;
  store->lock = openat(store->directory, "jobs.lock", O_RDWR | O_CREAT |
      O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (store->lock < 0 || !owned_file(store->lock) ||
      flock(store->lock, LOCK_EX | LOCK_NB) != 0) goto failed;
  store->healthy = 1;
  if (!discard_pending(store) || !load_journal(store) || !recover_journal(store)) goto failed;
  return 1;
failed:
  aula_device_jobs_close(store);
  return 0;
}

void aula_device_jobs_close(aula_device_jobs *store) {
  if (store == NULL) return;
  json_decref(store->document);
  if (store->lock >= 0) close(store->lock);
  if (store->directory >= 0) close(store->directory);
  *store = (aula_device_jobs){-1, -1, 0, NULL};
}

static json_t *project_job(json_t *job) {
  return json_pack("{s:s,s:s,s:s,s:b}", "id", job_text(job, "id"),
      "operation", job_text(job, "operation"), "state", job_text(job, "state"),
      "cancellable", strcmp(job_text(job, "state"), "queued") == 0);
}

static int existing_intent(json_t *jobs, const char *principal, const char *key,
    const char *operation, const char *fingerprint, json_t **output) {
  size_t index;
  json_t *job;
  json_array_foreach(jobs, index, job) {
    if (strcmp(job_text(job, "principal"), principal) != 0 ||
        strcmp(job_text(job, "key"), key) != 0) continue;
    if (strcmp(job_text(job, "operation"), operation) != 0 ||
        strcmp(job_text(job, "fingerprint"), fingerprint) != 0) return AULA_JOB_CONFLICT;
    *output = project_job(job);
    return *output == NULL ? AULA_JOB_ERROR : AULA_JOB_DUPLICATE;
  }
  return AULA_JOB_CREATED;
}

int aula_device_jobs_accept(aula_device_jobs *store, const char *principal,
    const char *key, const char *operation, const char *fingerprint, json_t **output) {
  json_t *jobs, *job;
  char id[24];
  int result;
  if (output == NULL) return AULA_JOB_ERROR;
  *output = NULL;
  if (store == NULL || !store->healthy || !job_token(principal, 64) ||
      !job_token(key, 64) || !job_token(operation, 48) ||
      !fingerprint_valid(fingerprint)) return AULA_JOB_ERROR;
  jobs = json_object_get(store->document, "jobs");
  result = existing_intent(jobs, principal, key, operation, fingerprint, output);
  if (result != AULA_JOB_CREATED) return result;
  if (json_array_size(jobs) >= AULA_DEVICE_JOB_LIMIT) return AULA_JOB_FULL;
  snprintf(id, sizeof(id), "job-%08u", (unsigned)json_array_size(jobs) + 1);
  job = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s}", "id", id, "principal", principal,
      "key", key, "operation", operation, "fingerprint", fingerprint, "state", "queued");
  if (job == NULL || json_array_append_new(jobs, job) != 0) return AULA_JOB_ERROR;
  if (!journal_write(store)) return AULA_JOB_ERROR;
  *output = project_job(json_array_get(jobs, json_array_size(jobs) - 1));
  return *output == NULL ? AULA_JOB_ERROR : AULA_JOB_CREATED;
}

static int transition_valid(const char *before, const char *after) {
  if (strcmp(before, "queued") == 0)
    return strcmp(after, "running") == 0 || strcmp(after, "cancelled") == 0;
  if (strcmp(before, "running") == 0)
    return strcmp(after, "uncertain") == 0 || strcmp(after, "succeeded") == 0 ||
        strcmp(after, "failed") == 0;
  if (strcmp(before, "uncertain") == 0)
    return strcmp(after, "succeeded") == 0 || strcmp(after, "failed") == 0;
  return 0;
}

int aula_device_jobs_transition(aula_device_jobs *store, const char *id,
    const char *state) {
  json_t *job;
  size_t index;
  if (store == NULL || !store->healthy || !job_token(id, 20) || !job_state(state)) return 0;
  json_array_foreach(json_object_get(store->document, "jobs"), index, job) {
    if (strcmp(job_text(job, "id"), id) != 0) continue;
    if (!transition_valid(job_text(job, "state"), state)) return 0;
    if (json_object_set_new(job, "state", json_string(state)) != 0) return 0;
    return journal_write(store);
  }
  return 0;
}

json_t *aula_device_jobs_list(const aula_device_jobs *store) {
  json_t *result, *job;
  size_t index;
  if (store == NULL || !store->healthy) return NULL;
  result = json_array();
  if (result == NULL) return NULL;
  json_array_foreach(json_object_get(store->document, "jobs"), index, job) {
    json_t *safe = project_job(job);
    if (safe == NULL || json_array_append_new(result, safe) != 0) {
      json_decref(result); return NULL;
    }
  }
  return result;
}
