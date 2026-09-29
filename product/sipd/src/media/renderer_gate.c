#include "aula_sipd/media_session.h"

static int direction_receives(aula_sdp_direction direction) {
  return direction == AULA_SDP_SENDRECV || direction == AULA_SDP_RECVONLY;
}

static aula_sdp_direction safe_without_renderer(aula_sdp_direction direction) {
  if (direction == AULA_SDP_SENDRECV) return AULA_SDP_SENDONLY;
  if (direction == AULA_SDP_RECVONLY) return AULA_SDP_INACTIVE;
  return direction;
}

static uint32_t required_renderer_capabilities(
    const aula_sdp_local_capabilities *requested) {
  uint32_t required = 0U;
  if (direction_receives(requested->video.direction) != 0) {
    required |= AULA_MEDIA_RENDER_H264_RX;
  }
  if (direction_receives(requested->audio.direction) != 0) {
    required |= AULA_MEDIA_RENDER_G711_RX;
  }
  return required;
}

static int renderer_is_ready(aula_media_renderer *renderer, uint32_t required) {
  aula_media_renderer_health health;
  if (aula_media_renderer_validate(renderer) != AULA_STATUS_OK ||
      renderer->vtable->reserve(renderer, required) != AULA_STATUS_OK ||
      renderer->vtable->start(renderer) != AULA_STATUS_OK ||
      renderer->vtable->health(renderer, &health) != AULA_STATUS_OK) {
    return 0;
  }
  return health.healthy != 0 && health.reserved != 0 &&
      (health.capabilities & required) == required;
}

static void release_renderer(aula_media_renderer *renderer) {
  if (renderer != NULL && renderer->vtable != NULL &&
      renderer->vtable->release != NULL) {
    renderer->vtable->release(renderer);
  }
}

aula_status aula_media_session_gate_renderer(
    aula_media_renderer *renderer,
    const aula_sdp_local_capabilities *requested_capabilities,
    aula_sdp_local_capabilities *out_capabilities) {
  uint32_t required;
  if (requested_capabilities == NULL || out_capabilities == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  *out_capabilities = *requested_capabilities;
  required = required_renderer_capabilities(requested_capabilities);
  if (required == 0U) return AULA_STATUS_OK;
  if (renderer_is_ready(renderer, required) != 0) return AULA_STATUS_OK;
  out_capabilities->video.direction = safe_without_renderer(requested_capabilities->video.direction);
  out_capabilities->audio.direction = safe_without_renderer(requested_capabilities->audio.direction);
  release_renderer(renderer);
  return AULA_STATUS_OK;
}
