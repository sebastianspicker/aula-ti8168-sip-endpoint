#ifndef AULA_DEVICE_LAUNCHER_H
#define AULA_DEVICE_LAUNCHER_H
#include <sys/types.h>
/* Root-only, fixed-path listener provisioning. Returns fd 3, retains its
 * process-lifetime lock, and recovers only an owned, provably stale socket. */
int aula_device_listener(gid_t gateway_group);
#endif
