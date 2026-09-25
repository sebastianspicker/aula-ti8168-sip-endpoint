static void pjsip_destroy(void *context, void *opaque) {
  (void)context;
  if (pjsip_callback_instance == opaque) pjsip_callback_instance = NULL;
  pjsip_release_instance((ls200_pjsip_instance *)opaque);
}

#include "pjsip_resolver_selection.inc"

/* The portable adapter selects a candidate with the operating-system
 * resolver, then invokes its scope authorizer before this driver may acquire a
 * transport. Do not attach pjlib-util's asynchronous DNS resolver: the pinned
 * revision predates a published response-validation fix. */
static ls200_status pjsip_resolve_hostname(const char *host, const char *service,
    ls200_sip_resolver_result *result,
    const ls200_sip_transport_info *preferred_peer) {
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *cursor;
  int status;
  ls200_status resolver_status = ls200_sip_resolver_prepare();
  if (resolver_status != LS200_STATUS_OK) {
    pjsip_log_failure("resolver-prepare", (int)resolver_status, 0);
    return resolver_status;
  }
  (void)memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  status = getaddrinfo(host, service, &hints, &addresses);
  if (status != 0 || addresses == NULL) {
    pjsip_log_failure("resolve-host", status, status == EAI_SYSTEM ? errno : 0);
    return LS200_STATUS_IO_ERROR;
  }
  cursor = pjsip_select_resolved_address(addresses, preferred_peer);
  if (cursor == NULL) {
    freeaddrinfo(addresses);
    return LS200_STATUS_IO_ERROR;
  }
  if (cursor->ai_family == AF_INET) {
    struct sockaddr_in address;
    (void)memcpy(&address, cursor->ai_addr, sizeof(address));
    result->peer.remote_address_length = 4U;
    (void)memcpy(result->peer.remote_address, &address.sin_addr, 4U);
    result->peer.remote_port = (uint16_t)ntohs(address.sin_port);
  } else if (cursor->ai_family == AF_INET6) {
    struct sockaddr_in6 address;
    (void)memcpy(&address, cursor->ai_addr, sizeof(address));
    result->peer.remote_address_length = 16U;
    (void)memcpy(result->peer.remote_address, &address.sin6_addr, 16U);
    result->peer.remote_port = (uint16_t)ntohs(address.sin6_port);
  }
  freeaddrinfo(addresses);
  return result->peer.remote_address_length == 0U ? LS200_STATUS_IO_ERROR : LS200_STATUS_OK;
}

static ls200_status pjsip_resolve_request(
    const ls200_pjsip_instance *instance,
    const ls200_sip_resolver_request *request, ls200_sip_resolver_result *result,
    const ls200_sip_transport_info *preferred_peer) {
  const char *at;
  const char *host_end;
  char host[256];
  char service[6];
  ls200_sip_dial_target parsed;
  ls200_status status;
  if (request == NULL || result == NULL || request->target_uri == NULL ||
      ls200_sip_parse_dial_target(request->target_uri, &parsed) != LS200_STATUS_OK)
    return LS200_STATUS_INVALID_ARGUMENT;
  at = strchr(request->target_uri + 4U, '@');
  host_end = at == NULL ? NULL : at + 1U;
  if (host_end == NULL || (size_t)(strcspn(host_end, ":;") + 1U) >= sizeof(host))
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(host, host_end, strcspn(host_end, ":;"));
  host[strcspn(host_end, ":;")] = '\0';
  (void)snprintf(service, sizeof(service), "%u", (unsigned int)parsed.port);
  status = instance == NULL ? LS200_STATUS_AGAIN :
      ls200_sip_crc_route_resolve_initial(
          &instance->crc_route, instance->profile, host, parsed.port,
          request->preferred_transport, result);
  if (status == LS200_STATUS_OK) return status;
  if (status != LS200_STATUS_AGAIN) return status;
  (void)memset(result, 0, sizeof(*result));
  status = pjsip_resolve_hostname(host, service, result, preferred_peer);
  if (status != LS200_STATUS_OK) return status;
  result->peer.transport = request->preferred_transport;
  result->peer.local_port = 5060U;
  result->is_fallback = 0;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_driver_resolve(
    void *context, void *opaque, const ls200_sip_resolver_request *request,
    ls200_sip_resolver_result *result) {
  (void)context;
  return pjsip_resolve_request((const ls200_pjsip_instance *)opaque, request,
                               result, NULL);
}

static ls200_status pjsip_reauthorize_transport(
    ls200_pjsip_instance *instance, const ls200_sip_transport_info *peer) {
  ls200_sip_driver_event event;
  if (instance == NULL || !ls200_sip_transport_is_valid(peer))
    return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&event, 0, sizeof(event));
  event.event.kind = LS200_SIP_EVENT_RESOLVED_PEER;
  event.event.transport = *peer;
  return instance->sink(instance->sink_context, &event);
}

#include "pjsip_uri_destination.inc"

static ls200_status pjsip_resolve_native_destination(
    const ls200_pjsip_instance *instance,
    const pjsip_native_uri_destination *destination,
    ls200_sip_resolver_result *result,
    const ls200_sip_transport_info *preferred_peer,
    int allow_configured_crc_route) {
  char service[6];
  ls200_status status;
  if (destination == NULL || result == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  (void)snprintf(service, sizeof(service), "%u",
                 (unsigned int)destination->port);
  status = allow_configured_crc_route == 0 || instance == NULL ?
      LS200_STATUS_AGAIN : ls200_sip_crc_route_resolve_initial(
          &instance->crc_route, instance->profile, destination->host,
          destination->port, destination->transport, result);
  if (status == LS200_STATUS_OK) return status;
  if (status != LS200_STATUS_AGAIN) return status;
  (void)memset(result, 0, sizeof(*result));
  status = pjsip_resolve_hostname(destination->host, service, result,
                                  preferred_peer);
  if (status != LS200_STATUS_OK) return status;
  result->peer.transport = destination->transport;
  result->peer.local_port = 5060U;
  result->is_fallback = 0;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_apply_pinned_fallback(
    const ls200_sip_resolver_result *result,
    const ls200_sip_transport_info *pinned_fallback_peer,
    ls200_sip_transport_info *out_peer, int *out_complete) {
  if (result == NULL || out_peer == NULL || out_complete == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  *out_complete = 0;
  if (pinned_fallback_peer == NULL) return LS200_STATUS_OK;
  if (result->peer.remote_address_length !=
          pinned_fallback_peer->remote_address_length ||
      result->peer.remote_port != pinned_fallback_peer->remote_port ||
      memcmp(result->peer.remote_address, pinned_fallback_peer->remote_address,
             result->peer.remote_address_length) != 0)
    return LS200_STATUS_SECURITY_ERROR;
  /* The only TCP destination allowed after fallback is the socket that was
   * authorized immediately before selection. Publishing it again as a generic
   * resolved peer would incorrectly apply the normal UDP policy. */
  if (result->peer.transport == LS200_TRANSPORT_TCP) {
    *out_peer = *pinned_fallback_peer;
    *out_complete = 1;
  }
  return LS200_STATUS_OK;
}

static ls200_status pjsip_authorize_uri_destination(
    ls200_pjsip_instance *instance, const pjsip_uri *uri,
    ls200_transport inherited_transport,
    const ls200_sip_transport_info *pinned_fallback_peer,
    const ls200_sip_transport_info *preferred_peer,
    ls200_sip_transport_info *out_peer, int allow_configured_crc_route) {
  pjsip_native_uri_destination destination;
  ls200_sip_resolver_result result;
  ls200_status status;
  int fallback_complete;
  if (instance == NULL || uri == NULL || out_peer == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_native_uri_destination_parse(uri, inherited_transport,
                                               &destination);
  pjsip_log_uri_metadata((int)status, destination.port_present,
                         destination.user_present, destination.lr_present,
                         destination.maddr_present, (int)inherited_transport,
                         destination.actual_transport);
  if (status != LS200_STATUS_OK) {
    pjsip_log_failure("authorize-uri-prepare", (int)status, 0);
    return status;
  }
  status = pjsip_resolve_native_destination(
      instance, &destination, &result, preferred_peer,
      allow_configured_crc_route);
  if (status != LS200_STATUS_OK) {
    pjsip_log_failure("authorize-uri-resolve", (int)status, 0);
    return status;
  }
  status = pjsip_apply_pinned_fallback(&result, pinned_fallback_peer, out_peer,
                                       &fallback_complete);
  if (status != LS200_STATUS_OK) {
    pjsip_log_failure("authorize-uri-fallback", (int)status, 0);
    return status;
  }
  if (fallback_complete != 0) return status;
  status = pjsip_reauthorize_transport(instance, &result.peer);
  if (status == LS200_STATUS_OK)
    *out_peer = pinned_fallback_peer == NULL ? result.peer : *pinned_fallback_peer;
  else
    pjsip_log_failure("authorize-uri-reauthorize", (int)status, 0);
  return status;
}

static ls200_status pjsip_authorize_dialog_routes(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_transport inherited_transport,
    const ls200_sip_transport_info *fallback_peer,
    ls200_sip_transport_info *out_next_hop,
    int allow_configured_crc_route) {
  pjsip_route_hdr *route;
  ls200_sip_transport_info target_peer;
  ls200_sip_transport_info route_peer;
  ls200_sip_transport_info next_hop;
  int has_route = 0;
  ls200_status status = pjsip_authorize_uri_destination(
      instance, dialog->dialog->target, inherited_transport, fallback_peer,
      &dialog->peer, &target_peer, allow_configured_crc_route);
  for (route = dialog->dialog->route_set.next;
       status == LS200_STATUS_OK && route != &dialog->dialog->route_set;
       route = route->next) {
    status = pjsip_authorize_uri_destination(
        instance, route->name_addr.uri, inherited_transport, fallback_peer,
        &dialog->peer, &route_peer, 0);
    if (status == LS200_STATUS_OK && has_route == 0) {
      next_hop = route_peer;
      has_route = 1;
    }
  }
  if (status == LS200_STATUS_OK)
    *out_next_hop = has_route != 0 ? next_hop : target_peer;
  return status;
}

static ls200_status pjsip_peer_to_sockaddr(
    const ls200_sip_transport_info *peer, pj_sockaddr *address, int *length);

static int pjsip_dialog_has_empty_frozen_route_set(
    const ls200_pjsip_dialog *dialog) {
  return dialog->dialog->route_set_frozen != 0 &&
         pj_list_empty(&dialog->dialog->route_set);
}

static int pjsip_dialog_has_live_pinned_tls_transport(
    const ls200_pjsip_dialog *dialog) {
  pj_sockaddr expected;
  int expected_length;
  return dialog->peer.transport == LS200_TRANSPORT_TLS &&
         dialog->transport != NULL &&
         PJSIP_TRANSPORT_IS_RELIABLE(dialog->transport) &&
         PJSIP_TRANSPORT_IS_SECURE(dialog->transport) &&
         dialog->transport->is_shutdown == 0 &&
         dialog->transport->is_destroying == 0 &&
         pjsip_peer_to_sockaddr(&dialog->peer, &expected, &expected_length) ==
             LS200_STATUS_OK &&
         expected_length == dialog->transport->addr_len &&
         pj_sockaddr_cmp(&dialog->transport->key.rem_addr, &expected) == 0;
}

static ls200_status pjsip_authorize_pinned_tls_target(
    ls200_pjsip_dialog *dialog, ls200_transport inherited_transport,
    ls200_sip_transport_info *out_next_hop) {
  pjsip_native_uri_destination destination;
  ls200_status status = pjsip_native_uri_destination_require_transport(
      dialog->dialog->target, inherited_transport, LS200_TRANSPORT_TLS,
      &destination);
  pjsip_log_uri_metadata((int)status, destination.port_present,
                         destination.user_present, destination.lr_present,
                         destination.maddr_present, (int)inherited_transport,
                         destination.actual_transport);
  if (status != LS200_STATUS_OK) {
    pjsip_log_failure("authorize-pinned-tls-target", (int)status, 0);
    return status;
  }
  if (!pjsip_dialog_has_live_pinned_tls_transport(dialog)) {
    pjsip_log_failure("authorize-pinned-tls-transport",
                      (int)LS200_STATUS_SECURITY_ERROR, 0);
    return LS200_STATUS_SECURITY_ERROR;
  }
  *out_next_hop = dialog->peer;
  return LS200_STATUS_OK;
}

static int pjsip_next_hop_matches_dialog(
    const ls200_sip_transport_info *next_hop,
    const ls200_pjsip_dialog *dialog) {
  return next_hop->transport == dialog->peer.transport &&
         next_hop->remote_address_length == dialog->peer.remote_address_length &&
         next_hop->remote_port == dialog->peer.remote_port &&
         memcmp(next_hop->remote_address, dialog->peer.remote_address,
                next_hop->remote_address_length) == 0;
}

static ls200_status pjsip_authorize_dialog_destinations_internal(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_sip_transport_info *out_next_hop, int allow_configured_crc_route) {
  ls200_sip_transport_info next_hop;
  const ls200_sip_transport_info *fallback_peer;
  ls200_transport inherited_transport;
  ls200_status status;
  if (instance == NULL || dialog == NULL || dialog->dialog == NULL ||
      out_next_hop == NULL || !ls200_sip_transport_is_valid(&dialog->peer))
    return LS200_STATUS_INVALID_ARGUMENT;
  fallback_peer = instance->allow_tcp_fallback != 0 &&
                          dialog->peer.transport == LS200_TRANSPORT_TCP ?
                      &dialog->peer : NULL;
  inherited_transport = fallback_peer != NULL ? LS200_TRANSPORT_UDP :
                                                dialog->peer.transport;
  pjsip_dlg_inc_lock(dialog->dialog);
  /* TCP fallback is authorized as the fixed original socket before it is
   * selected. Its logical target still has to satisfy the original UDP scope;
   * do not publish a second adapter-wide fallback event here, because only
   * the first event carries the dialog update that pins the replacement. */
  if (pjsip_dialog_has_empty_frozen_route_set(dialog) &&
      dialog->peer.transport == LS200_TRANSPORT_TLS) {
    /* The authenticated reliable transport is the network next hop. The
     * response Contact remains the dialog Request-URI, but cannot redirect
     * this ACK/BYE to another socket. A nonempty route set still follows the
     * full destination authorization path below. */
    status = pjsip_authorize_pinned_tls_target(dialog, inherited_transport,
                                                &next_hop);
  } else {
    status = pjsip_authorize_dialog_routes(instance, dialog,
                                           inherited_transport, fallback_peer,
                                           &next_hop,
                                           allow_configured_crc_route);
  }
  /* This low-level dialog already owns a selector for one acquired peer. A
   * changed Contact/Route is authorized for policy visibility, but is refused
   * unless it resolves to that exact peer: changing selectors here would
   * require atomic route, TLS-name, transaction and transport replacement.
   * Pinning this invariant prevents PJSIP from independently resolving a
   * post-authorization hostname to a different address. */
  if (status == LS200_STATUS_OK &&
      !pjsip_next_hop_matches_dialog(&next_hop, dialog)) {
    pjsip_log_peer_mismatch(&next_hop, &dialog->peer);
    status = LS200_STATUS_SECURITY_ERROR;
  }
  pjsip_dlg_dec_lock(dialog->dialog);
  if (status == LS200_STATUS_OK) *out_next_hop = next_hop;
  return status;
}

static ls200_status pjsip_authorize_dialog_destinations(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_sip_transport_info *out_next_hop) {
  return pjsip_authorize_dialog_destinations_internal(
      instance, dialog, out_next_hop, 0);
}

static ls200_status pjsip_authorize_initial_dialog_destinations(
    ls200_pjsip_instance *instance, ls200_pjsip_dialog *dialog,
    ls200_sip_transport_info *out_next_hop) {
  return pjsip_authorize_dialog_destinations_internal(
      instance, dialog, out_next_hop, 1);
}

static pjsip_transport_type_e pjsip_transport_for(ls200_transport transport) {
  return transport == LS200_TRANSPORT_TLS ? PJSIP_TRANSPORT_TLS :
         transport == LS200_TRANSPORT_TCP ? PJSIP_TRANSPORT_TCP :
         PJSIP_TRANSPORT_UDP;
}

static ls200_status pjsip_extract_target_host(const char *target_uri,
                                               char *host,
                                               size_t capacity) {
  const char *start;
  size_t length;
  if (target_uri == NULL || host == NULL || capacity == 0U ||
      ls200_sip_parse_dial_target(target_uri, &(ls200_sip_dial_target){0}) !=
          LS200_STATUS_OK)
    return LS200_STATUS_INVALID_ARGUMENT;
  start = strchr(target_uri + 4U, '@');
  if (start == NULL || start[1] == '\0') return LS200_STATUS_INVALID_DATA;
  ++start;
  length = strcspn(start, ":;");
  if (length == 0U || length >= capacity) return LS200_STATUS_INVALID_DATA;
  (void)memcpy(host, start, length);
  host[length] = '\0';
  return LS200_STATUS_OK;
}

static pj_status_t pjsip_acquire_authorized_transport(
    ls200_pjsip_instance *instance, const char *target_uri,
    const ls200_sip_transport_info *peer,
    const pj_sockaddr *remote, int remote_length, pjsip_transport **out_transport) {
  pjsip_tx_data *hint = NULL;
  pj_status_t status;
  char host[256];
  pj_str_t host_text;
  if (peer->transport != LS200_TRANSPORT_TLS) {
    return pjsip_endpt_acquire_transport(instance->endpoint,
                                         pjsip_transport_for(peer->transport),
                                         remote, remote_length, NULL, out_transport);
  }
  if (pjsip_extract_target_host(target_uri, host, sizeof(host)) !=
      LS200_STATUS_OK)
    return PJ_EINVAL;
  status = pjsip_endpt_create_tdata(instance->endpoint, &hint);
  if (status != PJ_SUCCESS) return status;
  pjsip_tx_data_add_ref(hint);
  host_text = pj_str(host);
  pj_strdup(hint->pool, &hint->dest_info.name, &host_text);
  status = pjsip_endpt_acquire_transport2(
      instance->endpoint, pjsip_transport_for(peer->transport), remote,
      remote_length, NULL, hint, out_transport);
  (void)pjsip_tx_data_dec_ref(hint);
  return status;
}

#include "pjsip_registration.inc"

#include "pjsip_address.inc"

static ls200_status pjsip_pin_dialog_request(
    pjsip_tx_data *request, const ls200_sip_transport_info *peer) {
  pj_sockaddr address;
  int address_length;
  char literal[PJ_INET6_ADDRSTRLEN];
  pj_str_t literal_text;
  ls200_status status;
  if (request == NULL || peer == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_peer_to_sockaddr(peer, &address, &address_length);
  if (status != LS200_STATUS_OK) return status;
  if (pj_inet_ntop(address.addr.sa_family, pj_sockaddr_get_addr(&address), literal,
                   sizeof(literal)) != PJ_SUCCESS)
    return LS200_STATUS_INVALID_DATA;
  literal_text = pj_str(literal);
  (void)memset(&request->dest_info, 0, sizeof(request->dest_info));
  pj_strdup(request->pool, &request->dest_info.name, &literal_text);
  request->dest_info.addr.count = 1U;
  request->dest_info.addr.entry[0].name = request->dest_info.name;
  request->dest_info.addr.entry[0].type = pjsip_transport_for(peer->transport);
  request->dest_info.addr.entry[0].priority = 0U;
  request->dest_info.addr.entry[0].weight = 0U;
  pj_sockaddr_cp(&request->dest_info.addr.entry[0].addr, &address);
  request->dest_info.addr.entry[0].addr_len = address_length;
  request->dest_info.cur_addr = 0U;
  return LS200_STATUS_OK;
}

static int pjsip_tx_destination_matches_dialog(
    const pjsip_tx_data *request, const ls200_pjsip_dialog *dialog) {
  pj_sockaddr expected;
  int expected_length;
  if (request == NULL || dialog == NULL || request->tp_info.transport == NULL ||
      request->tp_info.dst_addr_len <= 0 ||
      pjsip_peer_to_sockaddr(&dialog->peer, &expected, &expected_length) !=
          LS200_STATUS_OK)
    return 0;
  return request->tp_info.transport == dialog->transport &&
         (pjsip_transport_type_e)request->tp_info.transport->key.type ==
             pjsip_transport_for(dialog->peer.transport) &&
         request->tp_info.dst_addr_len == expected_length &&
         pj_sockaddr_cmp(&request->tp_info.dst_addr, &expected) == 0;
}
