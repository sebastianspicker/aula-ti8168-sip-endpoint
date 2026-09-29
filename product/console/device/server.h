#ifndef AULA_DEVICE_SERVER_H
#define AULA_DEVICE_SERVER_H
#include <jansson.h>
#include <stdint.h>
/* Companion-only serving half of the device protocol; never linked into the
 * unprivileged gateway. See client.h for the gateway-facing half. */
typedef json_t *(*aula_device_read_fn)(void);
typedef json_t *(*aula_device_dispatch_fn)(const char *operation, json_t *arguments,
    const char *correlation, const char **outcome);
int aula_device_serve_commands(int fd, uint32_t peer_uid, aula_device_read_fn read_status,
    aula_device_read_fn read_jobs, aula_device_dispatch_fn dispatch);
int aula_device_serve_request(int fd, aula_device_read_fn read_status);
/* Revision 2 adds correlated, bounded status and paginated job queries. */
int aula_device_serve_queries(int fd, aula_device_read_fn read_status,
    aula_device_read_fn read_jobs);
#endif
