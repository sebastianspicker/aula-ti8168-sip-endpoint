#ifndef LS200_SIPD_MEDIA_RENDERER_H
#define LS200_SIPD_MEDIA_RENDERER_H

#include "ls200_sipd/media.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_media_renderer_capability {
  LS200_MEDIA_RENDER_G711_RX = 1U << 0,
  LS200_MEDIA_RENDER_H264_RX = 1U << 1,
  LS200_MEDIA_RENDER_FAKE_AUDIO = 1U << 2,
  LS200_MEDIA_RENDER_FAKE_HDMI = 1U << 3
} ls200_media_renderer_capability;

typedef struct ls200_media_renderer_health {
  uint32_t capabilities;
  uint64_t frames_rendered;
  uint64_t frames_dropped;
  uint64_t output_fingerprint;
  uint64_t last_pts_ns;
  ls200_status last_error;
  int reserved;
  int healthy;
} ls200_media_renderer_health;

typedef struct ls200_media_renderer ls200_media_renderer;

typedef struct ls200_media_renderer_vtable {
  ls200_status (*reserve)(ls200_media_renderer *renderer, uint32_t required);
  ls200_status (*start)(ls200_media_renderer *renderer);
  ls200_status (*render)(ls200_media_renderer *renderer,
                         const ls200_media_frame *frame);
  ls200_status (*health)(const ls200_media_renderer *renderer,
                         ls200_media_renderer_health *out_health);
  void (*release)(ls200_media_renderer *renderer);
  void (*destroy)(ls200_media_renderer *renderer);
} ls200_media_renderer_vtable;

struct ls200_media_renderer {
  const ls200_media_renderer_vtable *vtable;
  void *context;
};

ls200_status ls200_media_renderer_validate(const ls200_media_renderer *renderer);
/* Deterministic QEMU/test renderer.  It accepts only the advertised media
 * forms, fingerprints bytes and PTS, and never claims physical playback. */
ls200_status ls200_fake_renderer_create(uint32_t capabilities,
                                        ls200_media_renderer *out_renderer);
/* Vendor arec adapters are intentionally unavailable until an exact vendor
 * ABI and device contract is supplied at build time. */
ls200_status ls200_vendor_arec_renderer_create(ls200_media_renderer *out_renderer);

#ifdef __cplusplus
}
#endif

#endif
