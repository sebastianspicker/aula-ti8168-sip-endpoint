#ifndef AULA_DEVICE_TRANSPORT_H
#define AULA_DEVICE_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>

#define AULA_DEVICE_SOCKET "/run/aula-device/control.sock"
#define AULA_DEVICE_LIMIT 4096U

int aula_device_connect(const char *path, uint32_t owner, unsigned timeout_ms);
int aula_device_peer(int fd, uint32_t owner);
int aula_device_transfer(int fd, void *buffer, size_t length, int writing,
                          uint64_t deadline);
uint64_t aula_device_clock(void);
#endif
