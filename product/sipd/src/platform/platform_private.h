#ifndef LS200_SIPD_PLATFORM_PRIVATE_H
#define LS200_SIPD_PLATFORM_PRIVATE_H

#include "ls200_sipd/platform.h"

#include <sys/types.h>

ls200_status ls200_platform_process_birth_token(pid_t pid, uint64_t *out_token);

#endif
