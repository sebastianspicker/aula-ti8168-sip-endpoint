#ifndef AULA_SIPD_MEDIA_RENDERER_H
#define AULA_SIPD_MEDIA_RENDERER_H

#include "aula_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_media_renderer_capability {
  AULA_MEDIA_RENDER_G711_RX = 1U << 0,
  AULA_MEDIA_RENDER_H264_RX = 1U << 1,
  AULA_MEDIA_RENDER_FAKE_AUDIO = 1U << 2,
  AULA_MEDIA_RENDER_FAKE_HDMI = 1U << 3
} aula_media_renderer_capability;

typedef struct aula_media_renderer_health {
  uint32_t capabilities;
  uint64_t frames_rendered;
  uint64_t frames_dropped;
  uint64_t output_fingerprint;
  uint64_t last_pts_ns;
  aula_status last_error;
  int reserved;
  int healthy;
} aula_media_renderer_health;

typedef struct aula_media_renderer aula_media_renderer;

typedef struct aula_media_renderer_vtable {
  aula_status (*reserve)(aula_media_renderer *renderer, uint32_t required);
  aula_status (*start)(aula_media_renderer *renderer);
  aula_status (*render)(aula_media_renderer *renderer,
                         const aula_media_frame *frame);
  aula_status (*health)(const aula_media_renderer *renderer,
                         aula_media_renderer_health *out_health);
  void (*release)(aula_media_renderer *renderer);
  void (*destroy)(aula_media_renderer *renderer);
} aula_media_renderer_vtable;

struct aula_media_renderer {
  const aula_media_renderer_vtable *vtable;
  void *context;
};

aula_status aula_media_renderer_validate(const aula_media_renderer *renderer);
/* Deterministic QEMU/test renderer.  It accepts only the advertised media
 * forms, fingerprints bytes and PTS, and never claims physical playback. */
aula_status aula_fake_renderer_create(uint32_t capabilities,
                                        aula_media_renderer *out_renderer);
/* Vendor vendor adapters are intentionally unavailable until an exact vendor
 * ABI and device contract is supplied at build time. */
aula_status aula_hw_renderer_create(aula_media_renderer *out_renderer);

#ifdef __cplusplus
}
#endif

#endif
