#ifndef LS200_SIPD_CRC_ROUTE_H
#define LS200_SIPD_CRC_ROUTE_H

#include "ls200_sipd/pjsip.h"

typedef struct ls200_sip_crc_route {
  uint8_t address[4];
  int present;
} ls200_sip_crc_route;

int ls200_sip_crc_address_is_valid(const char *address);
ls200_status ls200_sip_crc_route_initialize(ls200_sip_crc_route *route,
                                            const char *address);
ls200_status ls200_sip_crc_route_resolve_initial(
    const ls200_sip_crc_route *route, ls200_zoom_profile profile,
    const char *host, uint16_t port, ls200_transport transport,
    ls200_sip_resolver_result *out_result);
ls200_status ls200_pjsip_driver_configure_route(
    const ls200_pjsip_driver_options *options, const char *crc_address,
    ls200_sip_driver_config *out_config);

#endif
