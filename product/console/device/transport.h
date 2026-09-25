#ifndef LS200_DEVICE_TRANSPORT_H
#define LS200_DEVICE_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>

#define LS200_DEVICE_SOCKET "/run/ls200-device/control.sock"
#define LS200_OEM_SOCKET "/var/run/webapi/webapi.sock"
#define LS200_DEVICE_LIMIT 4096U

int ls200_device_connect(const char *path, uint32_t owner, unsigned timeout_ms);
int ls200_device_peer(int fd, uint32_t owner);
int ls200_device_transfer(int fd, void *buffer, size_t length, int writing,
                          uint64_t deadline);
uint64_t ls200_device_clock(void);
#endif
