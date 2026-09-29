#include "endpoint_media_policy.h"

#include "endpoint_internal.h"

#include <string.h>

static int endpoint_media_public_ipv4(const uint8_t address[4],
                                      uint16_t port) {
  aula_sip_transport_info target;
  (void)memset(&target, 0, sizeof(target));
  target.transport = AULA_TRANSPORT_UDP;
  target.local_port = 1U;
  target.remote_port = port;
  target.remote_address_length = 4U;
  (void)memcpy(target.remote_address, address, 4U);
  return aula_sip_transport_is_valid(&target) &&
      !aula_sip_transport_is_restricted_network(&target);
}

static int endpoint_direct_crc_media_policy(
    const aula_endpoint *endpoint, const uint8_t target[4], uint16_t port) {
  const aula_sip_transport_info *peer = &endpoint->authorized_sip_peer;
  return endpoint->sip_config.profile == AULA_ZOOM_PROFILE_DIRECT_CRC &&
      endpoint->view != NULL && endpoint->view->enable_public_network != 0 &&
      endpoint->view->enable_tls != 0 &&
      endpoint->view->media_security_policy == REQUIRE_SRTP &&
      peer->transport == AULA_TRANSPORT_TLS &&
      peer->remote_address_length == 4U &&
      !aula_sip_transport_is_restricted_network(peer) &&
      endpoint_media_public_ipv4(target, port);
}

int endpoint_media_policy_allows(const aula_endpoint *endpoint,
                                 const uint8_t target[4], uint16_t port) {
  const aula_sip_transport_info *peer;
  if (endpoint == NULL || target == NULL || port == 0U ||
      endpoint->has_authorized_sip_peer == 0) return 0;
  peer = &endpoint->authorized_sip_peer;
  if (!aula_sip_transport_is_valid(peer)) return 0;
  if (peer->remote_address_length == 4U &&
      memcmp(target, peer->remote_address, 4U) == 0) return 1;
  return endpoint_direct_crc_media_policy(endpoint, target, port);
}
