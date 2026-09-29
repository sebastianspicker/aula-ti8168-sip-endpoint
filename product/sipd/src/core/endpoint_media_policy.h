#ifndef AULA_SIPD_ENDPOINT_MEDIA_POLICY_H
#define AULA_SIPD_ENDPOINT_MEDIA_POLICY_H

#include <stdint.h>

struct aula_endpoint;

int endpoint_media_policy_allows(const struct aula_endpoint *endpoint,
                                 const uint8_t target[4], uint16_t port);

#endif
