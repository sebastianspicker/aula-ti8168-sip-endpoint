#ifndef AULA_RECEIVE_MONITOR_H
#define AULA_RECEIVE_MONITOR_H

#include "aula_sipd/media_renderer.h"

/* Headless local decoding; this adapter does not drive HDMI or speakers. */
aula_status aula_receive_monitor_create(const char *decoder_path,
                                           aula_media_renderer *out_renderer);

#endif
