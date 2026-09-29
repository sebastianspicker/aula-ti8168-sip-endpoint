static int pjsip_resolved_address_is_usable(const struct addrinfo *address) {
  return address->ai_addr != NULL &&
         ((address->ai_family == AF_INET &&
           address->ai_addrlen >= sizeof(struct sockaddr_in)) ||
          (address->ai_family == AF_INET6 &&
           address->ai_addrlen >= sizeof(struct sockaddr_in6)));
}

static int pjsip_resolved_address_matches_peer(
    const struct addrinfo *address, const aula_sip_transport_info *peer) {
  if (peer == NULL) return 0;
  if (address->ai_family == AF_INET && peer->remote_address_length == 4U) {
    struct sockaddr_in socket_address;
    (void)memcpy(&socket_address, address->ai_addr, sizeof(socket_address));
    return (uint16_t)ntohs(socket_address.sin_port) == peer->remote_port &&
           memcmp(&socket_address.sin_addr, peer->remote_address, 4U) == 0;
  }
  if (address->ai_family == AF_INET6 && peer->remote_address_length == 16U) {
    struct sockaddr_in6 socket_address;
    (void)memcpy(&socket_address, address->ai_addr, sizeof(socket_address));
    return (uint16_t)ntohs(socket_address.sin6_port) == peer->remote_port &&
           memcmp(&socket_address.sin6_addr, peer->remote_address, 16U) == 0;
  }
  return 0;
}

static struct addrinfo *pjsip_select_resolved_address(
    struct addrinfo *addresses, const aula_sip_transport_info *preferred_peer) {
  struct addrinfo *cursor;
  struct addrinfo *first = NULL;
  struct addrinfo *ipv4 = NULL;
  for (cursor = addresses; cursor != NULL; cursor = cursor->ai_next) {
    if (!pjsip_resolved_address_is_usable(cursor)) continue;
    if (first == NULL) first = cursor;
    if (ipv4 == NULL && cursor->ai_family == AF_INET) ipv4 = cursor;
    /* Keep an acquired socket only while its exact address and port remain
     * in the current answer set. DNS ordering must not change the peer.
     * The caller still authorizes the result and enforces the pinned socket. */
    if (pjsip_resolved_address_matches_peer(cursor, preferred_peer)) return cursor;
  }
  return ipv4 == NULL ? first : ipv4;
}
