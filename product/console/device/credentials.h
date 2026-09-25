#ifndef LS200_DEVICE_CREDENTIALS_H
#define LS200_DEVICE_CREDENTIALS_H
#include <jansson.h>
#include <stddef.h>
/* Argument-schema validation only; safe for the unprivileged gateway to link.
 * The persistent credential store lives in credentials_store.h and is
 * companion-only. */
int ls200_device_credential_text(json_t *value, size_t maximum);
int ls200_device_credentials_schema(json_t *arguments);
#endif
