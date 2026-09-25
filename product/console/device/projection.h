#ifndef LS200_DEVICE_PROJECTION_H
#define LS200_DEVICE_PROJECTION_H
#include <jansson.h>
/* Raw OEM responses never cross the companion boundary. */
json_t *ls200_device_project_recorder(json_t *raw);
int ls200_device_status_valid(json_t *value);
#endif
