#include "aula_sipd/media_renderer.h"

#include <stdlib.h>
#include <string.h>

#define AULA_RENDER_FNV_OFFSET UINT64_C(1469598103934665603)
#define AULA_RENDER_FNV_PRIME UINT64_C(1099511628211)

typedef struct aula_fake_renderer {
  aula_media_renderer_health health;
  int started;
} aula_fake_renderer;

static void renderer_hash(aula_fake_renderer *renderer, const uint8_t *data, size_t length) {
  size_t index;
  for (index = 0U; index < length; ++index) {
    renderer->health.output_fingerprint ^= data[index];
    renderer->health.output_fingerprint *= AULA_RENDER_FNV_PRIME;
  }
}

static aula_status fake_reserve(aula_media_renderer *renderer, uint32_t required) {
  aula_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || required == 0U ||
      (context->health.capabilities & required) != required) return AULA_STATUS_UNSUPPORTED;
  context->health.reserved = 1;
  context->health.healthy = 1;
  context->health.last_error = AULA_STATUS_OK;
  return AULA_STATUS_OK;
}

static aula_status fake_start(aula_media_renderer *renderer) {
  aula_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || context->health.reserved == 0) return AULA_STATUS_STATE_ERROR;
  context->started = 1;
  return AULA_STATUS_OK;
}

static aula_status fake_render(aula_media_renderer *renderer,
                                const aula_media_frame *frame) {
  aula_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  uint8_t pts[8];
  size_t index;
  if (context == NULL || frame == NULL || context->started == 0 ||
      frame->data.data == NULL || frame->data.length == 0U) return AULA_STATUS_INVALID_ARGUMENT;
  if ((frame->kind == AULA_MEDIA_AUDIO_PCM_S16LE &&
       (context->health.capabilities & AULA_MEDIA_RENDER_G711_RX) == 0U) ||
      (frame->kind == AULA_MEDIA_VIDEO_H264_ANNEX_B &&
       (context->health.capabilities & AULA_MEDIA_RENDER_H264_RX) == 0U)) {
    context->health.frames_dropped++;
    context->health.last_error = AULA_STATUS_UNSUPPORTED;
    context->health.healthy = 0;
    return AULA_STATUS_UNSUPPORTED;
  }
  for (index = 0U; index < sizeof(pts); ++index) pts[index] =
      (uint8_t)(frame->pts_ns >> (index * 8U));
  renderer_hash(context, pts, sizeof(pts));
  renderer_hash(context, frame->data.data, frame->data.length);
  context->health.frames_rendered++;
  context->health.last_pts_ns = frame->pts_ns;
  context->health.last_error = AULA_STATUS_OK;
  return AULA_STATUS_OK;
}

static aula_status fake_health(const aula_media_renderer *renderer,
                                aula_media_renderer_health *out_health) {
  const aula_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL || out_health == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_health = context->health;
  return AULA_STATUS_OK;
}

static void fake_release(aula_media_renderer *renderer) {
  aula_fake_renderer *context = renderer == NULL ? NULL : renderer->context;
  if (context == NULL) return;
  context->started = 0;
  context->health.reserved = 0;
  context->health.healthy = 0;
}

static void fake_destroy(aula_media_renderer *renderer) {
  if (renderer == NULL) return;
  free(renderer->context);
  renderer->context = NULL;
  renderer->vtable = NULL;
}

static const aula_media_renderer_vtable AULA_FAKE_RENDERER_VTABLE = {
  fake_reserve, fake_start, fake_render, fake_health, fake_release, fake_destroy
};

aula_status aula_media_renderer_validate(const aula_media_renderer *renderer) {
  if (renderer == NULL || renderer->vtable == NULL || renderer->context == NULL ||
      renderer->vtable->reserve == NULL || renderer->vtable->start == NULL ||
      renderer->vtable->render == NULL || renderer->vtable->health == NULL ||
      renderer->vtable->release == NULL || renderer->vtable->destroy == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  return AULA_STATUS_OK;
}

aula_status aula_fake_renderer_create(uint32_t capabilities,
                                        aula_media_renderer *out_renderer) {
  aula_fake_renderer *context;
  if (out_renderer == NULL || capabilities == 0U ||
      (capabilities & (uint32_t)~((uint32_t)AULA_MEDIA_RENDER_G711_RX |
                                  (uint32_t)AULA_MEDIA_RENDER_H264_RX |
                                  (uint32_t)AULA_MEDIA_RENDER_FAKE_AUDIO |
                                  (uint32_t)AULA_MEDIA_RENDER_FAKE_HDMI)) != 0U) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  context = calloc(1U, sizeof(*context));
  if (context == NULL) return AULA_STATUS_INTERNAL_ERROR;
  context->health.capabilities = capabilities;
  context->health.output_fingerprint = AULA_RENDER_FNV_OFFSET;
  context->health.last_error = AULA_STATUS_OK;
  out_renderer->vtable = &AULA_FAKE_RENDERER_VTABLE;
  out_renderer->context = context;
  return AULA_STATUS_OK;
}

aula_status aula_hw_renderer_create(aula_media_renderer *out_renderer) {
  if (out_renderer == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_renderer, 0, sizeof(*out_renderer));
  return AULA_STATUS_UNSUPPORTED;
}
