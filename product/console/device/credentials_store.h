#ifndef AULA_DEVICE_CREDENTIALS_STORE_H
#define AULA_DEVICE_CREDENTIALS_STORE_H
#include <jansson.h>
/* Persistent device credential store; companion-only, never linked into the
 * unprivileged gateway. Serialized by the companion event loop and its
 * lifetime state lock. */
int aula_device_credentials_open(int directory);
void aula_device_credentials_close(void);
json_t *aula_device_credentials_status(const char *connection);
json_t *aula_device_credentials_receipt(const char *key);
const char *aula_device_credentials_update(json_t *arguments, const char *key);
#endif
