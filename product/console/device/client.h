#ifndef LS200_DEVICE_CLIENT_H
#define LS200_DEVICE_CLIENT_H
#include <jansson.h>
#include <stddef.h>
/* The unprivileged gateway links only this client half of the device
 * protocol; companion serving lives in server.h. */
json_t *ls200_device_exchange(json_t *request);
int ls200_device_read_status(char *output, size_t capacity);
#endif
