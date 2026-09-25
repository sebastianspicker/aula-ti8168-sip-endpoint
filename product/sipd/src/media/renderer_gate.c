#include "ls200_sipd/media_session.h"

static int direction_receives(ls200_sdp_direction direction) {
  return direction == LS200_SDP_SENDRECV || direction == LS200_SDP_RECVONLY;
}

static ls200_sdp_direction safe_without_renderer(ls200_sdp_direction direction) {
  if (direction == LS200_SDP_SENDRECV) return LS200_SDP_SENDONLY;
  if (direction == LS200_SDP_RECVONLY) return LS200_SDP_INACTIVE;
  return direction;
}

static uint32_t required_renderer_capabilities(
    const ls200_sdp_local_capabilities *requested) {
  uint32_t required = 0U;
  if (direction_receives(requested->video.direction) != 0) {
    required |= LS200_MEDIA_RENDER_H264_RX;
  }
  if (direction_receives(requested->audio.direction) != 0) {
    required |= LS200_MEDIA_RENDER_G711_RX;
  }
  return required;
}

static int renderer_is_ready(ls200_media_renderer *renderer, uint32_t required) {
  ls200_media_renderer_health health;
  if (ls200_media_renderer_validate(renderer) != LS200_STATUS_OK ||
      renderer->vtable->reserve(renderer, required) != LS200_STATUS_OK ||
      renderer->vtable->start(renderer) != LS200_STATUS_OK ||
      renderer->vtable->health(renderer, &health) != LS200_STATUS_OK) {
    return 0;
  }
  return health.healthy != 0 && health.reserved != 0 &&
      (health.capabilities & required) == required;
}

static void release_renderer(ls200_media_renderer *renderer) {
  if (renderer != NULL && renderer->vtable != NULL &&
      renderer->vtable->release != NULL) {
    renderer->vtable->release(renderer);
  }
}

ls200_status ls200_media_session_gate_renderer(
    ls200_media_renderer *renderer,
    const ls200_sdp_local_capabilities *requested_capabilities,
    ls200_sdp_local_capabilities *out_capabilities) {
  uint32_t required;
  if (requested_capabilities == NULL || out_capabilities == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_capabilities = *requested_capabilities;
  required = required_renderer_capabilities(requested_capabilities);
  if (required == 0U) return LS200_STATUS_OK;
  if (renderer_is_ready(renderer, required) != 0) return LS200_STATUS_OK;
  out_capabilities->video.direction = safe_without_renderer(requested_capabilities->video.direction);
  out_capabilities->audio.direction = safe_without_renderer(requested_capabilities->audio.direction);
  release_renderer(renderer);
  return LS200_STATUS_OK;
}
