#ifndef LS200_PJSIP_READINESS_H
#define LS200_PJSIP_READINESS_H

#include "ls200_sipd/common.h"

ls200_status ls200_pjsip_media_readiness_sync(const int *descriptors,
                                               size_t descriptor_count);
void ls200_pjsip_media_readiness_cancel(void);

#endif
