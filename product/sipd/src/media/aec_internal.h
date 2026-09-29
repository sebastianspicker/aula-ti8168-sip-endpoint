#ifndef AULA_SIPD_AEC_INTERNAL_H
#define AULA_SIPD_AEC_INTERNAL_H

#include "aula_sipd/media_aec.h"

/* Test-only observability for fixed-delay reference selection. */
aula_status aula_aec_copy_delayed_reference(const aula_aec *aec,
                                              aula_mutable_bytes *out_frame);

#endif
