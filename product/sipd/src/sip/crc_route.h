#ifndef AULA_SIPD_CRC_ROUTE_H
#define AULA_SIPD_CRC_ROUTE_H

#include "aula_sipd/pjsip.h"

typedef struct aula_sip_crc_route {
  uint8_t address[4];
  int present;
} aula_sip_crc_route;

int aula_sip_crc_address_is_valid(const char *address);
aula_status aula_sip_crc_route_initialize(aula_sip_crc_route *route,
                                            const char *address);
aula_status aula_sip_crc_route_resolve_initial(
    const aula_sip_crc_route *route, aula_zoom_profile profile,
    const char *host, uint16_t port, aula_transport transport,
    aula_sip_resolver_result *out_result);
aula_status aula_pjsip_driver_configure_route(
    const aula_pjsip_driver_options *options, const char *crc_address,
    aula_sip_driver_config *out_config);

#endif
