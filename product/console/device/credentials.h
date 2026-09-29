#ifndef AULA_DEVICE_CREDENTIALS_H
#define AULA_DEVICE_CREDENTIALS_H
#include <jansson.h>
#include <stddef.h>
/* Argument-schema validation only; safe for the unprivileged gateway to link.
 * The persistent credential store lives in credentials_store.h and is
 * companion-only. */
int aula_device_credential_text(json_t *value, size_t maximum);
int aula_device_credentials_schema(json_t *arguments);
#endif
