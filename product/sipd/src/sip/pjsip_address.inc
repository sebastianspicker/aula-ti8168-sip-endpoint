static ls200_status pjsip_peer_to_sockaddr(const ls200_sip_transport_info *peer,
                                           pj_sockaddr *address, int *length) {
  pj_str_t text;
  char literal[INET6_ADDRSTRLEN];
  if (!ls200_sip_transport_is_valid(peer) || address == NULL || length == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (peer->remote_address_length != 4U && peer->remote_address_length != 16U)
    return LS200_STATUS_INVALID_DATA;
  (void)memset(literal, 0, sizeof(literal));
  if (pj_inet_ntop(peer->remote_address_length == 4U ? pj_AF_INET() : pj_AF_INET6(),
                   peer->remote_address, literal, sizeof(literal)) != PJ_SUCCESS)
    return LS200_STATUS_INVALID_DATA;
  /* PJLIB uses slen to distinguish a numeric address from a hostname. A
   * buffer capacity here makes it attempt DNS again for an authorized peer. */
  text = pj_str(literal);
  if (pj_sockaddr_init(peer->remote_address_length == 4U ? pj_AF_INET() : pj_AF_INET6(),
                       address, &text, peer->remote_port) != PJ_SUCCESS)
    return LS200_STATUS_IO_ERROR;
  *length = peer->remote_address_length == 4U ? (int)sizeof(pj_sockaddr_in) :
                                                 (int)sizeof(pj_sockaddr_in6);
  return LS200_STATUS_OK;
}
