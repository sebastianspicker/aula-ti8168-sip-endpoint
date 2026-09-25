#ifndef LS200_RECEIVE_MONITOR_H
#define LS200_RECEIVE_MONITOR_H

#include "ls200_sipd/media_renderer.h"

/* Headless local decoding; this adapter does not drive HDMI or speakers. */
ls200_status ls200_receive_monitor_create(const char *decoder_path,
                                           ls200_media_renderer *out_renderer);

#endif
