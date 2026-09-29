#ifndef AULA_PJSIP_READINESS_H
#define AULA_PJSIP_READINESS_H

#include "aula_sipd/common.h"

aula_status aula_pjsip_media_readiness_sync(const int *descriptors,
                                               size_t descriptor_count);
void aula_pjsip_media_readiness_cancel(void);

#endif
