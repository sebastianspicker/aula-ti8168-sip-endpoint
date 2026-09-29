#ifndef AULA_DEVICE_COMMANDS_H
#define AULA_DEVICE_COMMANDS_H
#include <jansson.h>
json_t *aula_device_dispatch(const char *operation, json_t *arguments,
    const char *correlation, const char **outcome);
#endif
