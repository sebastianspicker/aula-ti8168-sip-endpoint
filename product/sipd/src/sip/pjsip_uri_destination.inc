#define AULA_PJSIP_NATIVE_HOST_BYTES 256U

typedef struct pjsip_native_uri_destination {
  char host[AULA_PJSIP_NATIVE_HOST_BYTES];
  uint16_t port;
  aula_transport transport;
  int port_present;
  int user_present;
  int lr_present;
  int maddr_present;
  int actual_transport;
} pjsip_native_uri_destination;

static aula_status pjsip_uri_transport(const pjsip_uri *uri,
                                        aula_transport inherited,
                                        aula_transport *out_transport) {
  const pjsip_sip_uri *sip_uri;
  if (uri == NULL || out_transport == NULL ||
      (!PJSIP_URI_SCHEME_IS_SIP(uri) && !PJSIP_URI_SCHEME_IS_SIPS(uri)))
    return AULA_STATUS_INVALID_DATA;
  sip_uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(uri);
  if (sip_uri == NULL) return AULA_STATUS_INVALID_DATA;
  if (sip_uri->transport_param.slen == 0) {
    *out_transport = PJSIP_URI_SCHEME_IS_SIPS(uri) ? AULA_TRANSPORT_TLS :
                                                     inherited;
    return AULA_STATUS_OK;
  }
  if (pj_stricmp2(&sip_uri->transport_param, "udp") == 0)
    *out_transport = AULA_TRANSPORT_UDP;
  else if (pj_stricmp2(&sip_uri->transport_param, "tcp") == 0)
    *out_transport = AULA_TRANSPORT_TCP;
  else if (pj_stricmp2(&sip_uri->transport_param, "tls") == 0)
    *out_transport = AULA_TRANSPORT_TLS;
  else
    return AULA_STATUS_INVALID_DATA;
  if (PJSIP_URI_SCHEME_IS_SIPS(uri) && *out_transport != AULA_TRANSPORT_TLS)
    return AULA_STATUS_SECURITY_ERROR;
  return AULA_STATUS_OK;
}

static int pjsip_native_uri_parameters_supported(
    const pjsip_sip_uri *sip_uri) {
  return sip_uri->maddr_param.slen == 0 && sip_uri->ttl_param == -1 &&
         sip_uri->user_param.slen == 0 && sip_uri->method_param.slen == 0 &&
         pj_list_empty(&sip_uri->other_param) &&
         pj_list_empty(&sip_uri->header_param);
}

static aula_status pjsip_native_uri_copy_host(
    const pjsip_sip_uri *sip_uri,
    pjsip_native_uri_destination *out_destination) {
  if (sip_uri->host.ptr == NULL || sip_uri->host.slen <= 0 ||
      (size_t)sip_uri->host.slen >= sizeof(out_destination->host) ||
      memchr(sip_uri->host.ptr, '\0', (size_t)sip_uri->host.slen) != NULL)
    return AULA_STATUS_INVALID_DATA;
  (void)memcpy(out_destination->host, sip_uri->host.ptr,
               (size_t)sip_uri->host.slen);
  out_destination->host[sip_uri->host.slen] = '\0';
  return AULA_STATUS_OK;
}

static aula_status pjsip_native_uri_set_port(
    const pjsip_sip_uri *sip_uri,
    pjsip_native_uri_destination *out_destination) {
  if (sip_uri->port < 0 || sip_uri->port > 65535)
    return AULA_STATUS_INVALID_DATA;
  out_destination->port = sip_uri->port == 0 ?
      (out_destination->transport == AULA_TRANSPORT_TLS ? 5061U : 5060U) :
      (uint16_t)sip_uri->port;
  return AULA_STATUS_OK;
}

static aula_status pjsip_native_uri_destination_parse(
    const pjsip_uri *uri, aula_transport inherited_transport,
    pjsip_native_uri_destination *out_destination) {
  const pjsip_sip_uri *sip_uri;
  aula_status status;
  if (out_destination == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  (void)memset(out_destination, 0, sizeof(*out_destination));
  out_destination->actual_transport = -1;
  if (uri == NULL ||
      (!PJSIP_URI_SCHEME_IS_SIP(uri) && !PJSIP_URI_SCHEME_IS_SIPS(uri)))
    return AULA_STATUS_INVALID_DATA;
  sip_uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(uri);
  if (sip_uri == NULL) return AULA_STATUS_INVALID_DATA;
  out_destination->port_present = sip_uri->port != 0;
  out_destination->user_present = sip_uri->user.slen != 0;
  out_destination->lr_present = sip_uri->lr_param != 0;
  out_destination->maddr_present = sip_uri->maddr_param.slen != 0;
  status = pjsip_uri_transport(uri, inherited_transport,
                               &out_destination->transport);
  if (status != AULA_STATUS_OK) return status;
  out_destination->actual_transport = (int)out_destination->transport;
  if (!pjsip_native_uri_parameters_supported(sip_uri))
    return AULA_STATUS_UNSUPPORTED;
  status = pjsip_native_uri_copy_host(sip_uri, out_destination);
  return status == AULA_STATUS_OK ?
             pjsip_native_uri_set_port(sip_uri, out_destination) : status;
}

static aula_status pjsip_native_uri_destination_require_transport(
    const pjsip_uri *uri, aula_transport inherited_transport,
    aula_transport required_transport,
    pjsip_native_uri_destination *out_destination) {
  aula_status status = pjsip_native_uri_destination_parse(
      uri, inherited_transport, out_destination);
  if (status != AULA_STATUS_OK) return status;
  return out_destination->transport == required_transport ?
             AULA_STATUS_OK : AULA_STATUS_SECURITY_ERROR;
}
