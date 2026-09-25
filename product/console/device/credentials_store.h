#ifndef LS200_DEVICE_CREDENTIALS_STORE_H
#define LS200_DEVICE_CREDENTIALS_STORE_H
#include <jansson.h>
/* Persistent OEM credential store; companion-only, never linked into the
 * unprivileged gateway. Serialized by the companion event loop and its
 * lifetime state lock. */
int ls200_device_credentials_open(int directory);
void ls200_device_credentials_close(void);
json_t *ls200_device_credentials_status(const char *connection);
json_t *ls200_device_credentials_receipt(const char *key);
const char *ls200_device_credentials_update(json_t *arguments, const char *key);
char *ls200_device_credentials_login(void);
#endif
