#ifndef AULA_DEVICE_CLIENT_H
#define AULA_DEVICE_CLIENT_H
#include <jansson.h>
#include <stddef.h>
#include <stdint.h>
/* The unprivileged gateway links only this client half of the device
 * protocol; companion serving lives in server.h. */
json_t *aula_device_exchange(json_t *request);
json_t *aula_device_exchange_at(const char *path, uint32_t owner, json_t *request);
int aula_device_read_status_at(const char *path, uint32_t owner, char *output, size_t capacity);
/* Connected socket stays owned by the caller. Peer verification is mandatory. */
int aula_device_read_status_fd(int fd, uint32_t owner, char *output, size_t capacity);
int aula_device_read_status(char *output, size_t capacity);
#endif
