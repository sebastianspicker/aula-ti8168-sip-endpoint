#ifndef LS200_SIPD_AEC_INTERNAL_H
#define LS200_SIPD_AEC_INTERNAL_H

#include "ls200_sipd/media_aec.h"

/* Test-only observability for fixed-delay reference selection. */
ls200_status ls200_aec_copy_delayed_reference(const ls200_aec *aec,
                                              ls200_mutable_bytes *out_frame);

#endif
