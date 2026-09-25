#define _POSIX_C_SOURCE 200809L
#include "receive_monitor.h"
#include "receive_monitor_worker.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct receive_monitor {
  char *path;
  ls200_monitor_worker video;
  ls200_monitor_worker audio;
  ls200_media_renderer_health health;
  int started;
} receive_monitor;

static ls200_status flush_monitor(receive_monitor *monitor) {
  ls200_status video = ls200_monitor_worker_flush(&monitor->video);
  ls200_status audio = ls200_monitor_worker_flush(&monitor->audio);
  return video != LS200_STATUS_OK ? video : audio;
}

static void release_monitor(ls200_media_renderer *renderer) {
  receive_monitor *monitor = renderer->context;
  ls200_monitor_worker_stop(&monitor->video);
  ls200_monitor_worker_stop(&monitor->audio);
  monitor->started = 0;
  monitor->health.reserved = 0;
  monitor->health.healthy = 0;
}

static ls200_status reserve_monitor(ls200_media_renderer *renderer, uint32_t required) {
  receive_monitor *monitor = renderer->context;
  ls200_status status;
  if (required == 0U || (required & monitor->health.capabilities) != required)
    return LS200_STATUS_UNSUPPORTED;
  if (monitor->health.reserved) return flush_monitor(monitor);
  status = ls200_monitor_worker_start(&monitor->video, monitor->path, 1);
  if (status == LS200_STATUS_OK)
    status = ls200_monitor_worker_start(&monitor->audio, monitor->path, 0);
  if (status != LS200_STATUS_OK) {
    (void)fprintf(stderr, "receive monitor unavailable: status=%d\n", (int)status);
    release_monitor(renderer);
    return status;
  }
  monitor->health.reserved = 1;
  monitor->health.healthy = 1;
  return LS200_STATUS_OK;
}

static ls200_status start_monitor(ls200_media_renderer *renderer) {
  receive_monitor *monitor = renderer->context;
  ls200_status status;
  if (!monitor->health.reserved) return LS200_STATUS_STATE_ERROR;
  status = flush_monitor(monitor);
  monitor->started = status == LS200_STATUS_OK;
  return status;
}

static ls200_status render_monitor(ls200_media_renderer *renderer, const ls200_media_frame *frame) {
  receive_monitor *monitor = renderer->context;
  ls200_monitor_worker *worker;
  ls200_status status;
  if (frame == NULL || frame->data.data == NULL || frame->data.length == 0U)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (!monitor->started) return LS200_STATUS_STATE_ERROR;
  if (frame->kind == LS200_MEDIA_VIDEO_H264_ANNEX_B) worker = &monitor->video;
  else if (frame->kind == LS200_MEDIA_AUDIO_PCM_S16LE &&
           frame->sample_rate == 48000U && frame->channels == 2U) worker = &monitor->audio;
  else return LS200_STATUS_UNSUPPORTED;
  status = flush_monitor(monitor);
  if (status == LS200_STATUS_OK) status = ls200_monitor_worker_submit(worker, frame->data);
  monitor->health.last_error = status;
  monitor->health.healthy = status == LS200_STATUS_OK || status == LS200_STATUS_LIMIT_EXCEEDED;
  if (status == LS200_STATUS_OK) {
    ++monitor->health.frames_rendered;
    monitor->health.last_pts_ns = frame->pts_ns;
  } else ++monitor->health.frames_dropped;
  return status;
}

static ls200_status health_monitor(const ls200_media_renderer *renderer,
                                    ls200_media_renderer_health *out_health) {
  receive_monitor *monitor = renderer->context;
  if (out_health == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_health = monitor->health;
  out_health->healthy = monitor->health.reserved && flush_monitor(monitor) == LS200_STATUS_OK;
  return LS200_STATUS_OK;
}

static void destroy_monitor(ls200_media_renderer *renderer) {
  receive_monitor *monitor = renderer->context;
  release_monitor(renderer);
  free(monitor->path);
  free(monitor);
  renderer->context = NULL;
  renderer->vtable = NULL;
}

static const ls200_media_renderer_vtable monitor_vtable = {
    reserve_monitor, start_monitor, render_monitor, health_monitor, release_monitor, destroy_monitor};

ls200_status ls200_receive_monitor_create(const char *decoder_path,
                                           ls200_media_renderer *out_renderer) {
  receive_monitor *monitor;
  if (decoder_path == NULL || decoder_path[0] != '/' || strlen(decoder_path) > 4096U ||
      out_renderer == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (geteuid() == 0U || getegid() == 0U) return LS200_STATUS_PERMISSION_DENIED;
  monitor = calloc(1U, sizeof(*monitor));
  if (monitor == NULL) return LS200_STATUS_INTERNAL_ERROR;
  monitor->path = malloc(strlen(decoder_path) + 1U);
  if (monitor->path == NULL) { free(monitor); return LS200_STATUS_INTERNAL_ERROR; }
  (void)memcpy(monitor->path, decoder_path, strlen(decoder_path) + 1U);
  monitor->video.input_fd = monitor->audio.input_fd = -1;
  monitor->health.capabilities = LS200_MEDIA_RENDER_H264_RX | LS200_MEDIA_RENDER_G711_RX;
  out_renderer->context = monitor;
  out_renderer->vtable = &monitor_vtable;
  return LS200_STATUS_OK;
}
