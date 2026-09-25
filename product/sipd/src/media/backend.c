#include "ls200_sipd/backend.h"

ls200_status ls200_media_backend_validate(const ls200_media_backend *backend) {
  if (backend == NULL || backend->vtable == NULL || backend->context == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  if (backend->vtable->open == NULL || backend->vtable->start == NULL ||
      backend->vtable->read_video_access_unit == NULL ||
      backend->vtable->read_audio_pcm == NULL ||
      backend->vtable->request_video_keyframe == NULL ||
      backend->vtable->health == NULL || backend->vtable->stop == NULL ||
      backend->vtable->close == NULL) {
    return LS200_STATUS_CONFIGURATION_ERROR;
  }
  return LS200_STATUS_OK;
}
