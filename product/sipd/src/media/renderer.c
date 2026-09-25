#include "ls200_sipd/media_renderer.h"

#include <stdlib.h>
#include <string.h>

#define LS200_RENDER_FNV_OFFSET UINT64_C(1469598103934665603)
#define LS200_RENDER_FNV_PRIME UINT64_C(1099511628211)

typedef struct ls200_fake_renderer {
  ls200_media_renderer_health health;
  int started;
} ls200_fake_renderer;

static void renderer_hash(ls200_fake_renderer *renderer, const uint8_t *data, size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    renderer->health.output_fingerprint ^= data[index];
    renderer->health.output_fingerprint *= LS200_RENDER_FNV_PRIME;
  }
}

static ls200_status fake_reserve(ls200_media_renderer *renderer, uint32_t required) {
  ls200_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || required == 0U ||
      (context->health.capabilities & required) != required) return LS200_STATUS_UNSUPPORTED;
  context->health.reserved = 1;
  context->health.healthy = 1;
  context->health.last_error = LS200_STATUS_OK;
  return LS200_STATUS_OK;
}

static ls200_status fake_start(ls200_media_renderer *renderer) {
  ls200_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || context->health.reserved == 0) return LS200_STATUS_STATE_ERROR;
  context->started = 1;
  return LS200_STATUS_OK;
}

static ls200_status fake_render(ls200_media_renderer *renderer,
                                const ls200_media_frame *frame) {
  ls200_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  uint8_t pts[8];
  size_t index;
  if (context == NULL || frame == NULL || context->started == 0 ||
      frame->data.data == NULL || frame->data.length == 0U) return LS200_STATUS_INVALID_ARGUMENT;
  if ((frame->kind == LS200_MEDIA_AUDIO_PCM_S16LE &&
       (context->health.capabilities & LS200_MEDIA_RENDER_G711_RX) == 0U) ||
      (frame->kind == LS200_MEDIA_VIDEO_H264_ANNEX_B &&
       (context->health.capabilities & LS200_MEDIA_RENDER_H264_RX) == 0U)) {
    context->health.frames_dropped++;
    context->health.last_error = LS200_STATUS_UNSUPPORTED;
    context->health.healthy = 0;
    return LS200_STATUS_UNSUPPORTED;
  }
  for (index = 0U; index < sizeof(pts); ++index) pts[index] =
      (uint8_t)(frame->pts_ns >> (index * 8U));
  renderer_hash(context, pts, sizeof(pts));
  renderer_hash(context, frame->data.data, frame->data.length);
  context->health.frames_rendered++;
  context->health.last_pts_ns = frame->pts_ns;
  context->health.last_error = LS200_STATUS_OK;
  return LS200_STATUS_OK;
}

static ls200_status fake_health(const ls200_media_renderer *renderer,
                                ls200_media_renderer_health *out_health) {
  const ls200_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || out_health == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_health = context->health;
  return LS200_STATUS_OK;
}

static void fake_release(ls200_media_renderer *renderer) {
  ls200_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL) return;
  context->started = 0;
  context->health.reserved = 0;
  context->health.healthy = 0;
}

static void fake_destroy(ls200_media_renderer *renderer) {
  if (renderer == NULL) return;
  free(renderer->context);
  renderer->context = NULL;
  renderer->vtable = NULL;
}

static const ls200_media_renderer_vtable LS200_FAKE_RENDERER_VTABLE = {
  fake_reserve, fake_start, fake_render, fake_health, fake_release, fake_destroy
};

ls200_status ls200_media_renderer_validate(const ls200_media_renderer *renderer) {
  if (renderer == NULL || renderer->vtable == NULL || renderer->context == NULL ||
      renderer->vtable->reserve == NULL || renderer->vtable->start == NULL ||
      renderer->vtable->render == NULL || renderer->vtable->health == NULL ||
      renderer->vtable->release == NULL || renderer->vtable->destroy == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_fake_renderer_create(uint32_t capabilities,
                                        ls200_media_renderer *out_renderer) {
  ls200_fake_renderer *context;
  if (out_renderer == NULL || capabilities == 0U ||
      (capabilities & (uint32_t)~((uint32_t)LS200_MEDIA_RENDER_G711_RX |
                                  (uint32_t)LS200_MEDIA_RENDER_H264_RX |
                                  (uint32_t)LS200_MEDIA_RENDER_FAKE_AUDIO |
                                  (uint32_t)LS200_MEDIA_RENDER_FAKE_HDMI)) != 0U) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  context = calloc(1U, sizeof(*context));
  if (context == NULL) return LS200_STATUS_INTERNAL_ERROR;
  context->health.capabilities = capabilities;
  context->health.output_fingerprint = LS200_RENDER_FNV_OFFSET;
  context->health.last_error = LS200_STATUS_OK;
  out_renderer->vtable = &LS200_FAKE_RENDERER_VTABLE;
  out_renderer->context = context;
  return LS200_STATUS_OK;
}

ls200_status ls200_vendor_arec_renderer_create(ls200_media_renderer *out_renderer) {
  if (out_renderer == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_renderer, 0, sizeof(*out_renderer));
  return LS200_STATUS_UNSUPPORTED;
}
