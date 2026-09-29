#ifndef AULA_SIPD_PLATFORM_PRIVATE_H
#define AULA_SIPD_PLATFORM_PRIVATE_H

#include "aula_sipd/platform.h"

#include <sys/types.h>

aula_status aula_platform_process_birth_token(pid_t pid, uint64_t *out_token);

#endif
