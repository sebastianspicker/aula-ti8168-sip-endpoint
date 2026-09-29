#include "crc_route.h"

#include "adapter_internal.h"

#include <arpa/inet.h>
#include <string.h>

static int crc_address_parse(const char *address, uint8_t out_address[4]) {
  aula_sip_transport_info peer;
  if (address == NULL || address[0] == '\0' || out_address == NULL ||
      strlen(address) >= 16U)
    return 0;
  (void)memset(&peer, 0, sizeof(peer));
  peer.transport = AULA_TRANSPORT_TLS;
  peer.local_port = 5060U;
  peer.remote_port = 5061U;
  peer.remote_address_length = 4U;
  if (inet_pton(AF_INET, address, peer.remote_address) != 1 ||
      aula_sip_transport_is_restricted_network(&peer))
    return 0;
  (void)memcpy(out_address, peer.remote_address, 4U);
  return 1;
}

int aula_sip_crc_address_is_valid(const char *address) {
  uint8_t parsed[4];
  return address == NULL || address[0] == '\0' ||
      crc_address_parse(address, parsed);
}

aula_status aula_sip_crc_route_initialize(aula_sip_crc_route *route,
                                            const char *address) {
  if (route == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(route, 0, sizeof(*route));
  if (address == NULL || address[0] == '\0') return AULA_STATUS_OK;
  if (!crc_address_parse(address, route->address))
    return AULA_STATUS_INVALID_DATA;
  route->present = 1;
  return AULA_STATUS_OK;
}

aula_status aula_sip_crc_route_resolve_initial(
    const aula_sip_crc_route *route, aula_zoom_profile profile,
    const char *host, uint16_t port, aula_transport transport,
    aula_sip_resolver_result *out_result) {
  if (route == NULL || host == NULL || out_result == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (route->present == 0 || profile != AULA_ZOOM_PROFILE_DIRECT_CRC ||
      strcmp(host, AULA_ZOOM_CRC_DOMAIN) != 0 || port != 5061U ||
      transport != AULA_TRANSPORT_TLS)
    return AULA_STATUS_AGAIN;
  (void)memset(out_result, 0, sizeof(*out_result));
  out_result->peer.transport = AULA_TRANSPORT_TLS;
  out_result->peer.local_port = 5060U;
  out_result->peer.remote_port = 5061U;
  out_result->peer.remote_address_length = 4U;
  (void)memcpy(out_result->peer.remote_address, route->address, 4U);
  return AULA_STATUS_OK;
}
