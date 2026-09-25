#ifndef LS200_DEVICE_PROTOCOL_H
#define LS200_DEVICE_PROTOCOL_H
#include <jansson.h>
#include <stdint.h>
typedef json_t *(*ls200_device_read_fn)(void);
typedef json_t *(*ls200_device_dispatch_fn)(const char *operation, json_t *arguments,
    const char *correlation, const char **outcome);
int ls200_device_serve_commands(int fd, uint32_t peer_uid, ls200_device_read_fn read_status,
    ls200_device_read_fn read_jobs, ls200_device_dispatch_fn dispatch);
json_t *ls200_device_exchange(json_t *request);
int ls200_device_serve_request(int fd, ls200_device_read_fn read_status);
/* Revision 2 adds correlated, bounded status and paginated job queries. */
int ls200_device_serve_queries(int fd, ls200_device_read_fn read_status,
    ls200_device_read_fn read_jobs);
int ls200_device_read_status(char *output, size_t capacity);
#endif
