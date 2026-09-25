#define _POSIX_C_SOURCE 200809L
#include "ls200_sipd/backend.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct ls200_fixture_context {
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
  ls200_media_health health;
} ls200_fixture_context;

#define LS200_FIXTURE_FNV_OFFSET UINT64_C(1469598103934665603)
#define LS200_FIXTURE_FNV_PRIME UINT64_C(1099511628211)

static void ls200_fixture_hash(ls200_fixture_context *context, const uint8_t *data,
                               size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    context->fingerprint ^= data[index];
    context->fingerprint *= LS200_FIXTURE_FNV_PRIME;
  }
}

static int ls200_fixture_safe_path(const char *path) {
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

static void ls200_fixture_set_error(ls200_fixture_context *context,
                                    ls200_status status) {
  context->health.last_error = status;
}

static FILE *ls200_fixture_duplicate_input(int descriptor) {
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

static int ls200_fixture_config_name_and_paths_valid(const ls200_backend_config *config) {
  return config != NULL && config->name != NULL &&
      strcmp(config->name, "fixture") == 0 &&
      ls200_fixture_safe_path(config->video_source) &&
      ls200_fixture_safe_path(config->audio_source);
}

static int ls200_fixture_limits_valid(const ls200_backend_config *config) {
  return config->maximum_access_unit_bytes != 0U &&
      config->maximum_access_unit_bytes <= LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES &&
      config->maximum_pcm_frame_bytes >= 320U &&
      config->maximum_pcm_frame_bytes <= LS200_SIPD_MAX_AUDIO_FRAME_BYTES &&
      (config->use_inherited_fixture_fds == 0 || config->use_inherited_fixture_fds == 1) &&
      (config->use_inherited_fixture_fds == 0 ||
       (config->fixture_video_fd >= 0 && config->fixture_audio_fd >= 0));
}

static void ls200_fixture_open_inputs(ls200_fixture_context *context,
                                      const ls200_backend_config *config) {
  if (config->use_inherited_fixture_fds != 0) {
    context->video_file = ls200_fixture_duplicate_input(config->fixture_video_fd);
    context->audio_file = ls200_fixture_duplicate_input(config->fixture_audio_fd);
    return;
  }
  context->video_file = fopen(config->video_source, "rb");
  context->audio_file = fopen(config->audio_source, "rb");
}

static int ls200_fixture_allocate_buffers(ls200_fixture_context *context,
                                          const ls200_backend_config *config) {
  context->video_buffer = (uint8_t *)malloc(config->maximum_access_unit_bytes);
  context->audio_buffer = (uint8_t *)malloc(config->maximum_pcm_frame_bytes);
  return context->video_buffer != NULL && context->audio_buffer != NULL;
}

static ls200_status ls200_fixture_open(ls200_media_backend *backend,
                                       const ls200_backend_config *config) {
  ls200_fixture_context *context;
  if (backend == NULL || !ls200_fixture_config_name_and_paths_valid(config)) {
    return LS200_STATUS_CONFIGURATION_ERROR;
  }
  context = (ls200_fixture_context *)backend->context;
  if (context == NULL || context->opened != 0 || !ls200_fixture_limits_valid(config)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  /* The private-lab runner retains reviewed descriptors, so this path never
   * reopens their configured names and cannot be redirected by a pathname swap. */
  ls200_fixture_open_inputs(context, config);
  (void)ls200_fixture_allocate_buffers(context, config);
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
    ls200_fixture_set_error(context, LS200_STATUS_IO_ERROR);
    return LS200_STATUS_IO_ERROR;
  }
  context->video_capacity = config->maximum_access_unit_bytes;
  context->audio_capacity = config->maximum_pcm_frame_bytes;
  context->fingerprint = LS200_FIXTURE_FNV_OFFSET;
  context->opened = 1;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_start(ls200_media_backend *backend) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0 || context->started != 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  context->started = 1;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_read_video(ls200_media_backend *backend,
                                             ls200_deadline deadline,
                                             ls200_media_frame *out_frame) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
  size_t count;
  (void)deadline;
  if (context == NULL || out_frame == NULL || context->started == 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  if (context->video_complete != 0) {
    return LS200_STATUS_END;
  }
  count = fread(context->video_buffer, 1U, context->video_capacity, context->video_file);
  if (count == 0U) {
    context->video_complete = 1;
    if (ferror(context->video_file) != 0) {
      ls200_fixture_set_error(context, LS200_STATUS_IO_ERROR);
      return LS200_STATUS_IO_ERROR;
    }
    return LS200_STATUS_END;
  }
  if (!feof(context->video_file)) {
    context->health.frames_dropped++;
    ls200_fixture_set_error(context, LS200_STATUS_LIMIT_EXCEEDED);
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  out_frame->kind = LS200_MEDIA_VIDEO_H264_ANNEX_B;
  out_frame->data.data = context->video_buffer;
  out_frame->data.length = count;
  out_frame->pts_ns = context->video_pts_ns;
  out_frame->sample_rate = 0U;
  out_frame->channels = 0U;
  out_frame->keyframe = 1;
  context->video_complete = 1;
  ls200_fixture_hash(context, (const uint8_t *)"V", 1U);
  ls200_fixture_hash(context, context->video_buffer, count);
  context->video_pts_ns += 33333333ULL;
  context->health.frames_read++;
  context->health.bytes_read += count;
  context->health.last_success_ns = out_frame->pts_ns;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_read_audio(ls200_media_backend *backend,
                                             ls200_deadline deadline,
                                             ls200_media_frame *out_frame) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
  size_t count;
  (void)deadline;
  if (context == NULL || out_frame == NULL || context->started == 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  count = fread(context->audio_buffer, 1U, 320U, context->audio_file);
  if (count == 0U) {
    if (ferror(context->audio_file) != 0) {
      ls200_fixture_set_error(context, LS200_STATUS_IO_ERROR);
      return LS200_STATUS_IO_ERROR;
    }
    return LS200_STATUS_END;
  }
  if (count != 320U) {
    context->health.frames_dropped++;
    ls200_fixture_set_error(context, LS200_STATUS_INVALID_DATA);
    return LS200_STATUS_INVALID_DATA;
  }
  out_frame->kind = LS200_MEDIA_AUDIO_PCM_S16LE;
  out_frame->data.data = context->audio_buffer;
  out_frame->data.length = count;
  out_frame->pts_ns = context->audio_pts_ns;
  out_frame->sample_rate = 8000U;
  out_frame->channels = 1U;
  out_frame->keyframe = 0;
  ls200_fixture_hash(context, (const uint8_t *)"A", 1U);
  ls200_fixture_hash(context, context->audio_buffer, count);
  context->audio_pts_ns += 20000000ULL;
  context->health.frames_read++;
  context->health.bytes_read += count;
  context->health.last_success_ns = out_frame->pts_ns;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_request_keyframe(ls200_media_backend *backend) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
  if (context == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (context->opened == 0 || context->started == 0 ||
      context->video_file == NULL) {
    return LS200_STATUS_STATE_ERROR;
  }
  clearerr(context->video_file);
  if (fseek(context->video_file, 0L, SEEK_SET) != 0) {
    ls200_fixture_set_error(context, LS200_STATUS_IO_ERROR);
    return LS200_STATUS_IO_ERROR;
  }
  context->video_complete = 0;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_health(const ls200_media_backend *backend,
                                         ls200_media_health *out_health) {
  const ls200_fixture_context *context = backend == NULL ? NULL :
      (const ls200_fixture_context *)backend->context;
  if (context == NULL || out_health == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_health = context->health;
  return LS200_STATUS_OK;
}

static ls200_status ls200_fixture_stop(ls200_media_backend *backend) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0) {
    return LS200_STATUS_STATE_ERROR;
  }
  context->started = 0;
  return LS200_STATUS_OK;
}

static void ls200_fixture_close(ls200_media_backend *backend) {
  ls200_fixture_context *context = backend == NULL ? NULL :
      (ls200_fixture_context *)backend->context;
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

static const ls200_media_backend_vtable LS200_FIXTURE_VTABLE = {
  ls200_fixture_open,
  ls200_fixture_start,
  ls200_fixture_read_video,
  ls200_fixture_read_audio,
  ls200_fixture_request_keyframe,
  ls200_fixture_health,
  ls200_fixture_stop,
  ls200_fixture_close
};

ls200_status ls200_fixture_backend_create(ls200_media_backend *out_backend) {
  ls200_fixture_context *context;
  if (out_backend == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  context = (ls200_fixture_context *)calloc(1U, sizeof(*context));
  if (context == NULL) {
    return LS200_STATUS_INTERNAL_ERROR;
  }
  out_backend->vtable = &LS200_FIXTURE_VTABLE;
  out_backend->context = context;
  return LS200_STATUS_OK;
}

ls200_status ls200_fixture_backend_fingerprint(const ls200_media_backend *backend,
                                               uint64_t *out_fingerprint) {
  const ls200_fixture_context *context;
  if (backend == NULL || out_fingerprint == NULL || backend->vtable != &LS200_FIXTURE_VTABLE) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  context = (const ls200_fixture_context *)backend->context;
  if (context == NULL || context->opened == 0) return LS200_STATUS_STATE_ERROR;
  *out_fingerprint = context->fingerprint;
  return LS200_STATUS_OK;
}
