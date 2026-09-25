#ifndef LS200_SIPD_ENDPOINT_MEDIA_POLICY_H
#define LS200_SIPD_ENDPOINT_MEDIA_POLICY_H

#include <stdint.h>

struct ls200_endpoint;

int endpoint_media_policy_allows(const struct ls200_endpoint *endpoint,
                                 const uint8_t target[4], uint16_t port);

#endif
