#include "aula_sipd/backend.h"

aula_status aula_media_backend_validate(const aula_media_backend *backend) {
  if (backend == NULL || backend->vtable == NULL || backend->context == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (backend->vtable->open == NULL || backend->vtable->start == NULL ||
      backend->vtable->read_video_access_unit == NULL ||
      backend->vtable->read_audio_pcm == NULL ||
      backend->vtable->request_video_keyframe == NULL ||
      backend->vtable->health == NULL || backend->vtable->stop == NULL ||
      backend->vtable->close == NULL) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  return AULA_STATUS_OK;
}
