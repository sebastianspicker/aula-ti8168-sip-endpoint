#define LS200_PJSIP_NATIVE_HOST_BYTES 256U

typedef struct pjsip_native_uri_destination {
  char host[LS200_PJSIP_NATIVE_HOST_BYTES];
  uint16_t port;
  ls200_transport transport;
  int port_present;
  int user_present;
  int lr_present;
  int maddr_present;
  int actual_transport;
} pjsip_native_uri_destination;

static ls200_status pjsip_uri_transport(const pjsip_uri *uri,
                                        ls200_transport inherited,
                                        ls200_transport *out_transport) {
  const pjsip_sip_uri *sip_uri;
  if (uri == NULL || out_transport == NULL ||
      (!PJSIP_URI_SCHEME_IS_SIP(uri) && !PJSIP_URI_SCHEME_IS_SIPS(uri)))
    return LS200_STATUS_INVALID_DATA;
  sip_uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(uri);
  if (sip_uri == NULL) return LS200_STATUS_INVALID_DATA;
  if (sip_uri->transport_param.slen == 0) {
    *out_transport = PJSIP_URI_SCHEME_IS_SIPS(uri) ? LS200_TRANSPORT_TLS :
                                                     inherited;
    return LS200_STATUS_OK;
  }
  if (pj_stricmp2(&sip_uri->transport_param, "udp") == 0)
    *out_transport = LS200_TRANSPORT_UDP;
  else if (pj_stricmp2(&sip_uri->transport_param, "tcp") == 0)
    *out_transport = LS200_TRANSPORT_TCP;
  else if (pj_stricmp2(&sip_uri->transport_param, "tls") == 0)
    *out_transport = LS200_TRANSPORT_TLS;
  else
    return LS200_STATUS_INVALID_DATA;
  if (PJSIP_URI_SCHEME_IS_SIPS(uri) && *out_transport != LS200_TRANSPORT_TLS)
    return LS200_STATUS_SECURITY_ERROR;
  return LS200_STATUS_OK;
}

static int pjsip_native_uri_parameters_supported(
    const pjsip_sip_uri *sip_uri) {
  return sip_uri->maddr_param.slen == 0 && sip_uri->ttl_param == -1 &&
         sip_uri->user_param.slen == 0 && sip_uri->method_param.slen == 0 &&
         pj_list_empty(&sip_uri->other_param) &&
         pj_list_empty(&sip_uri->header_param);
}

static ls200_status pjsip_native_uri_copy_host(
    const pjsip_sip_uri *sip_uri,
    pjsip_native_uri_destination *out_destination) {
  if (sip_uri->host.ptr == NULL || sip_uri->host.slen <= 0 ||
      (size_t)sip_uri->host.slen >= sizeof(out_destination->host) ||
      memchr(sip_uri->host.ptr, '\0', (size_t)sip_uri->host.slen) != NULL)
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(out_destination->host, sip_uri->host.ptr,
               (size_t)sip_uri->host.slen);
  out_destination->host[sip_uri->host.slen] = '\0';
  return LS200_STATUS_OK;
}

static ls200_status pjsip_native_uri_set_port(
    const pjsip_sip_uri *sip_uri,
    pjsip_native_uri_destination *out_destination) {
  if (sip_uri->port < 0 || sip_uri->port > 65535)
    return LS200_STATUS_INVALID_DATA;
  out_destination->port = sip_uri->port == 0 ?
      (out_destination->transport == LS200_TRANSPORT_TLS ? 5061U : 5060U) :
      (uint16_t)sip_uri->port;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_native_uri_destination_parse(
    const pjsip_uri *uri, ls200_transport inherited_transport,
    pjsip_native_uri_destination *out_destination) {
  const pjsip_sip_uri *sip_uri;
  ls200_status status;
  if (out_destination == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_destination, 0, sizeof(*out_destination));
  out_destination->actual_transport = -1;
  if (uri == NULL ||
      (!PJSIP_URI_SCHEME_IS_SIP(uri) && !PJSIP_URI_SCHEME_IS_SIPS(uri)))
    return LS200_STATUS_INVALID_DATA;
  sip_uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(uri);
  if (sip_uri == NULL) return LS200_STATUS_INVALID_DATA;
  out_destination->port_present = sip_uri->port != 0;
  out_destination->user_present = sip_uri->user.slen != 0;
  out_destination->lr_present = sip_uri->lr_param != 0;
  out_destination->maddr_present = sip_uri->maddr_param.slen != 0;
  status = pjsip_uri_transport(uri, inherited_transport,
                               &out_destination->transport);
  if (status != LS200_STATUS_OK) return status;
  out_destination->actual_transport = (int)out_destination->transport;
  if (!pjsip_native_uri_parameters_supported(sip_uri))
    return LS200_STATUS_UNSUPPORTED;
  status = pjsip_native_uri_copy_host(sip_uri, out_destination);
  return status == LS200_STATUS_OK ?
             pjsip_native_uri_set_port(sip_uri, out_destination) : status;
}

static ls200_status pjsip_native_uri_destination_require_transport(
    const pjsip_uri *uri, ls200_transport inherited_transport,
    ls200_transport required_transport,
    pjsip_native_uri_destination *out_destination) {
  ls200_status status = pjsip_native_uri_destination_parse(
      uri, inherited_transport, out_destination);
  if (status != LS200_STATUS_OK) return status;
  return out_destination->transport == required_transport ?
             LS200_STATUS_OK : LS200_STATUS_SECURITY_ERROR;
}
