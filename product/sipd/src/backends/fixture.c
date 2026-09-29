#define _POSIX_C_SOURCE 200809L
#include "aula_sipd/backend.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct aula_fixture_context {
  FILE *video_file;
  FILE *audio_file;
  uint8_t *video_buffer;
  uint8_t *audio_buffer;
  size_t video_capacity;
  size_t audio_capacity;
  uint64_t video_pts_ns;
  uint64_t audio_pts_ns;
  uint64_t fingerprint;
  int opened;
  int started;
  int video_complete;
  aula_media_health health;
} aula_fixture_context;

#define AULA_FIXTURE_FNV_OFFSET UINT64_C(1469598103934665603)
#define AULA_FIXTURE_FNV_PRIME UINT64_C(1099511628211)

static void aula_fixture_hash(aula_fixture_context *context, const uint8_t *data,
                               size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    context->fingerprint ^= data[index];
    context->fingerprint *= AULA_FIXTURE_FNV_PRIME;
  }
}

static int aula_fixture_safe_path(const char *path) {
  const char *cursor;
  if (path == NULL || path[0] == '\0') {
    return 0;
  }
  for (cursor = path; *cursor != '\0'; cursor++) {
    unsigned char character = (unsigned char)*cursor;
    if (character < 0x20U || character == 0x7fU || character == '\\') {
      return 0;
    }
  }
  return strstr(path, "..") == NULL;
}

static void aula_fixture_set_error(aula_fixture_context *context,
                                    aula_status status) {
  context->health.last_error = status;
}

static FILE *aula_fixture_duplicate_input(int descriptor) {
  struct stat details;
  int duplicate;
  if (descriptor < 0 || fstat(descriptor, &details) != 0 ||
      !S_ISREG(details.st_mode)) {
    return NULL;
  }
  duplicate = dup(descriptor);
  if (duplicate < 0 || fcntl(duplicate, F_SETFD, FD_CLOEXEC) != 0) {
    if (duplicate >= 0) (void)close(duplicate);
    return NULL;
  }
  return fdopen(duplicate, "rb");
}

static int aula_fixture_config_name_and_paths_valid(const aula_backend_config *config) {
  return config != NULL && config->name != NULL &&
      strcmp(config->name, "fixture") == 0 &&
      aula_fixture_safe_path(config->video_source) &&
      aula_fixture_safe_path(config->audio_source);
}

static int aula_fixture_limits_valid(const aula_backend_config *config) {
  return config->maximum_access_unit_bytes != 0U &&
      config->maximum_access_unit_bytes <= AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES &&
      config->maximum_pcm_frame_bytes >= 320U &&
      config->maximum_pcm_frame_bytes <= AULA_SIPD_MAX_AUDIO_FRAME_BYTES &&
      (config->use_inherited_fixture_fds == 0 || config->use_inherited_fixture_fds == 1) &&
      (config->use_inherited_fixture_fds == 0 ||
       (config->fixture_video_fd >= 0 && config->fixture_audio_fd >= 0));
}

static void aula_fixture_open_inputs(aula_fixture_context *context,
                                      const aula_backend_config *config) {
  if (config->use_inherited_fixture_fds != 0) {
    context->video_file = aula_fixture_duplicate_input(config->fixture_video_fd);
    context->audio_file = aula_fixture_duplicate_input(config->fixture_audio_fd);
    return;
  }
  context->video_file = fopen(config->video_source, "rb");
  context->audio_file = fopen(config->audio_source, "rb");
}

static int aula_fixture_allocate_buffers(aula_fixture_context *context,
                                          const aula_backend_config *config) {
  context->video_buffer = (uint8_t *)malloc(config->maximum_access_unit_bytes);
  context->audio_buffer = (uint8_t *)malloc(config->maximum_pcm_frame_bytes);
  return context->video_buffer != NULL && context->audio_buffer != NULL;
}

static aula_status aula_fixture_open(aula_media_backend *backend,
                                       const aula_backend_config *config) {
  aula_fixture_context *context;
  if (backend == NULL || !aula_fixture_config_name_and_paths_valid(config)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  context = (aula_fixture_context *)backend->context;
  if (context == NULL || context->opened != 0 || !aula_fixture_limits_valid(config)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  /* The private-lab runner retains reviewed descriptors, so this path never
   * reopens their configured names and cannot be redirected by a pathname swap. */
  aula_fixture_open_inputs(context, config);
  (void)aula_fixture_allocate_buffers(context, config);
  if (context->video_file == NULL || context->audio_file == NULL ||
      context->video_buffer == NULL || context->audio_buffer == NULL) {
    if (context->video_file != NULL) {
      (void)fclose(context->video_file);
    }
    if (context->audio_file != NULL) {
      (void)fclose(context->audio_file);
    }
    free(context->video_buffer);
    free(context->audio_buffer);
    context->video_file = NULL;
    context->audio_file = NULL;
    context->video_buffer = NULL;
    context->audio_buffer = NULL;
    aula_fixture_set_error(context, AULA_STATUS_IO_ERROR);
    return AULA_STATUS_IO_ERROR;
  }
  context->video_capacity = config->maximum_access_unit_bytes;
  context->audio_capacity = config->maximum_pcm_frame_bytes;
  context->fingerprint = AULA_FIXTURE_FNV_OFFSET;
  context->opened = 1;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_start(aula_media_backend *backend) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0 || context->started != 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  context->started = 1;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_read_video(aula_media_backend *backend,
                                             aula_deadline deadline,
                                             aula_media_frame *out_frame) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  size_t count;
  (void)deadline;
  if (context == NULL || out_frame == NULL || context->started == 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (context->video_complete != 0) {
    return AULA_STATUS_END;
  }
  count = fread(context->video_buffer, 1U, context->video_capacity, context->video_file);
  if (count == 0U) {
    context->video_complete = 1;
    if (ferror(context->video_file) != 0) {
      aula_fixture_set_error(context, AULA_STATUS_IO_ERROR);
      return AULA_STATUS_IO_ERROR;
    }
    return AULA_STATUS_END;
  }
  if (!feof(context->video_file)) {
    context->health.frames_dropped++;
    aula_fixture_set_error(context, AULA_STATUS_LIMIT_EXCEEDED);
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  out_frame->kind = AULA_MEDIA_VIDEO_H264_ANNEX_B;
  out_frame->data.data = context->video_buffer;
  out_frame->data.length = count;
  out_frame->pts_ns = context->video_pts_ns;
  out_frame->sample_rate = 0U;
  out_frame->channels = 0U;
  out_frame->keyframe = 1;
  context->video_complete = 1;
  aula_fixture_hash(context, (const uint8_t *)"V", 1U);
  aula_fixture_hash(context, context->video_buffer, count);
  context->video_pts_ns += 33333333ULL;
  context->health.frames_read++;
  context->health.bytes_read += count;
  context->health.last_success_ns = out_frame->pts_ns;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_read_audio(aula_media_backend *backend,
                                             aula_deadline deadline,
                                             aula_media_frame *out_frame) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  size_t count;
  (void)deadline;
  if (context == NULL || out_frame == NULL || context->started == 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  count = fread(context->audio_buffer, 1U, 320U, context->audio_file);
  if (count == 0U) {
    if (ferror(context->audio_file) != 0) {
      aula_fixture_set_error(context, AULA_STATUS_IO_ERROR);
      return AULA_STATUS_IO_ERROR;
    }
    return AULA_STATUS_END;
  }
  if (count != 320U) {
    context->health.frames_dropped++;
    aula_fixture_set_error(context, AULA_STATUS_INVALID_DATA);
    return AULA_STATUS_INVALID_DATA;
  }
  out_frame->kind = AULA_MEDIA_AUDIO_PCM_S16LE;
  out_frame->data.data = context->audio_buffer;
  out_frame->data.length = count;
  out_frame->pts_ns = context->audio_pts_ns;
  out_frame->sample_rate = 8000U;
  out_frame->channels = 1U;
  out_frame->keyframe = 0;
  aula_fixture_hash(context, (const uint8_t *)"A", 1U);
  aula_fixture_hash(context, context->audio_buffer, count);
  context->audio_pts_ns += 20000000ULL;
  context->health.frames_read++;
  context->health.bytes_read += count;
  context->health.last_success_ns = out_frame->pts_ns;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_request_keyframe(aula_media_backend *backend) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  if (context == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (context->opened == 0 || context->started == 0 ||
      context->video_file == NULL) {
    return AULA_STATUS_STATE_ERROR;
  }
  clearerr(context->video_file);
  if (fseek(context->video_file, 0L, SEEK_SET) != 0) {
    aula_fixture_set_error(context, AULA_STATUS_IO_ERROR);
    return AULA_STATUS_IO_ERROR;
  }
  context->video_complete = 0;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_health(const aula_media_backend *backend,
                                         aula_media_health *out_health) {
  const aula_fixture_context *context = backend == NULL ? NULL :
      (const aula_fixture_context *)backend->context;
  if (context == NULL || out_health == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_health = context->health;
  return AULA_STATUS_OK;
}

static aula_status aula_fixture_stop(aula_media_backend *backend) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0) {
    return AULA_STATUS_STATE_ERROR;
  }
  context->started = 0;
  return AULA_STATUS_OK;
}

static void aula_fixture_close(aula_media_backend *backend) {
  aula_fixture_context *context = backend == NULL ? NULL :
      (aula_fixture_context *)backend->context;
  if (context == NULL) {
    return;
  }
  if (context->video_file != NULL) {
    (void)fclose(context->video_file);
  }
  if (context->audio_file != NULL) {
    (void)fclose(context->audio_file);
  }
  free(context->video_buffer);
  free(context->audio_buffer);
  free(context);
  backend->context = NULL;
  backend->vtable = NULL;
}

static const aula_media_backend_vtable AULA_FIXTURE_VTABLE = {
  aula_fixture_open,
  aula_fixture_start,
  aula_fixture_read_video,
  aula_fixture_read_audio,
  aula_fixture_request_keyframe,
  aula_fixture_health,
  aula_fixture_stop,
  aula_fixture_close
};

aula_status aula_fixture_backend_create(aula_media_backend *out_backend) {
  aula_fixture_context *context;
  if (out_backend == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  context = (aula_fixture_context *)calloc(1U, sizeof(*context));
  if (context == NULL) {
    return AULA_STATUS_INTERNAL_ERROR;
  }
  out_backend->vtable = &AULA_FIXTURE_VTABLE;
  out_backend->context = context;
  return AULA_STATUS_OK;
}

aula_status aula_fixture_backend_fingerprint(const aula_media_backend *backend,
                                               uint64_t *out_fingerprint) {
  const aula_fixture_context *context;
  if (backend == NULL || out_fingerprint == NULL || backend->vtable != &AULA_FIXTURE_VTABLE) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  context = (const aula_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0) return AULA_STATUS_STATE_ERROR;
  *out_fingerprint = context->fingerprint;
  return AULA_STATUS_OK;
}
