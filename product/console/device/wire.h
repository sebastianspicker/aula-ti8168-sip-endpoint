#ifndef LS200_DEVICE_WIRE_H
#define LS200_DEVICE_WIRE_H
#include <jansson.h>
#include <stdint.h>
/* Length-prefixed JSON framing shared by the client and companion-server
 * halves of the device protocol. Carries no privileged state of its own. */
json_t *ls200_device_receive_message(int fd, uint64_t deadline);
int ls200_device_send_message(int fd, json_t *message, uint64_t deadline);
#endif
