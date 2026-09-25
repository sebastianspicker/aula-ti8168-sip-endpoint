static int pjsip_copy_registration_challenge_text(
    const pjsip_digest_challenge *digest, char *realm, char *nonce,
    char *opaque, char *algorithm, char *qop) {
  if (!pjsip_copy_text(realm, LS200_PJSIP_DIGEST_FIELD_BYTES, digest->realm))
    return 0;
  if (!pjsip_copy_text(nonce, LS200_PJSIP_DIGEST_FIELD_BYTES, digest->nonce))
    return 0;
  if (!pjsip_copy_text(algorithm, LS200_PJSIP_DIGEST_FIELD_BYTES,
                       digest->algorithm))
    return 0;
  if (!pjsip_copy_text(qop, LS200_PJSIP_DIGEST_FIELD_BYTES, digest->qop))
    return 0;
  if (strcmp(algorithm, "SHA-256") != 0 || strcmp(qop, "auth") != 0)
    return 0;
  if (digest->opaque.slen != 0 &&
      !pjsip_copy_text(opaque, LS200_PJSIP_DIGEST_FIELD_BYTES, digest->opaque))
    return 0;
  if (digest->opaque.slen == 0) opaque[0] = '\0';
  return 1;
}

static int pjsip_registration_challenge(
    const pjsip_rx_data *rdata, ls200_sip_digest_challenge *challenge,
    char *realm, char *nonce, char *opaque, char *algorithm, char *qop) {
  pjsip_hdr_e type;
  pjsip_www_authenticate_hdr *header;
  const pjsip_digest_challenge *digest;
  int proxy;
  if (rdata == NULL || rdata->msg_info.msg == NULL || challenge == NULL)
    return 0;
  proxy = rdata->msg_info.msg->line.status.code == 407;
  type = proxy ? PJSIP_H_PROXY_AUTHENTICATE : PJSIP_H_WWW_AUTHENTICATE;
  header = (pjsip_www_authenticate_hdr *)
      pjsip_msg_find_hdr(rdata->msg_info.msg, type, NULL);
  if (header == NULL || pj_stricmp2(&header->scheme, "Digest") != 0) return 0;
  digest = &header->challenge.digest;
  if (!pjsip_copy_registration_challenge_text(digest, realm, nonce, opaque,
                                              algorithm, qop))
    return 0;
  challenge->realm = realm;
  challenge->nonce = nonce;
  challenge->opaque = opaque[0] == '\0' ? NULL : opaque;
  challenge->algorithm = algorithm;
  challenge->qop = qop;
  challenge->proxy_challenge = proxy;
  return 1;
}

static void pjsip_clear_registration_credentials(pjsip_auth_clt_sess *session,
                                                  pjsip_cred_info *credential) {
  unsigned index;
  if (session == NULL || credential == NULL) return;
  for (index = 0; index < session->cred_cnt; ++index) {
    if (session->cred_info[index].data.ptr != NULL &&
        session->cred_info[index].data.slen > 0)
      pjsip_wipe(session->cred_info[index].data.ptr,
                 (size_t)session->cred_info[index].data.slen);
  }
  (void)pjsip_auth_clt_set_credentials(session, 0, credential);
}

static int pjsip_registration_can_retry(
    const ls200_pjsip_instance *instance,
    const pjsip_auth_clt_async_on_chal_param *parameters,
    ls200_sip_digest_challenge *challenge, char *realm, char *nonce,
    char *opaque, char *algorithm, char *qop) {
  return instance->registration_credential_provider != NULL &&
         instance->auth_username[0] != '\0' &&
         instance->registration_digest_attempts < instance->registration_digest_limit &&
         pjsip_registration_challenge(parameters->rdata, challenge, realm, nonce,
                                      opaque, algorithm, qop);
}

static pj_status_t pjsip_registration_prepare_retry(
    pjsip_auth_clt_sess *session, ls200_pjsip_instance *instance,
    const pjsip_auth_clt_async_on_chal_param *parameters,
    const ls200_sip_digest_challenge *challenge, const char *realm,
    ls200_mutable_bytes *secret, pjsip_cred_info *credential,
    pjsip_tx_data **retry) {
  ls200_status credential_status = instance->registration_credential_provider(
      instance->registration_credential_context, challenge, secret);
  if (credential_status != LS200_STATUS_OK || secret->length == 0U ||
      secret->length > secret->capacity)
    return PJ_EINVAL;
  ++instance->registration_digest_attempts;
  credential->realm = pj_str((char *)realm);
  credential->scheme = pj_str("digest");
  credential->username = pj_str(instance->auth_username);
  credential->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
  credential->data.ptr = (char *)secret->data;
  credential->data.slen = (pj_ssize_t)secret->length;
  credential->algorithm_type = PJSIP_AUTH_ALGORITHM_SHA256;
  if (pjsip_auth_clt_set_credentials(session, 1, credential) != PJ_SUCCESS)
    return PJ_EINVAL;
  return pjsip_auth_clt_reinit_req(session, parameters->rdata, parameters->tdata,
                                   retry);
}

static pj_bool_t pjsip_abandon_registration_challenge(
    pjsip_auth_clt_sess *session, void *token, ls200_pjsip_instance *instance,
    const pjsip_auth_clt_async_on_chal_param *parameters, uint8_t *secret_data,
    size_t secret_data_size) {
  if (parameters->rdata != NULL && parameters->rdata->msg_info.msg != NULL) {
    instance->registration_status_code =
        parameters->rdata->msg_info.msg->line.status.code;
    instance->registration_expiration = 0U;
    instance->registration_active = 0;
  }
  pjsip_wipe(secret_data, secret_data_size);
  (void)pjsip_auth_clt_async_abandon(session, token);
  return PJ_TRUE;
}

static pj_bool_t pjsip_on_registration_challenge(
    pjsip_auth_clt_sess *session, void *token,
    const pjsip_auth_clt_async_on_chal_param *parameters) {
  ls200_pjsip_instance *instance;
  uint8_t secret_data[512];
  ls200_mutable_bytes secret = {secret_data, sizeof(secret_data), 0U};
  ls200_sip_digest_challenge challenge;
  pjsip_cred_info credential;
  pjsip_tx_data *retry = NULL;
  char realm[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char nonce[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char opaque[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char algorithm[LS200_PJSIP_DIGEST_FIELD_BYTES];
  char qop[LS200_PJSIP_DIGEST_FIELD_BYTES];
  pj_status_t status = PJ_EINVAL;
  if (session == NULL || token == NULL || parameters == NULL ||
      parameters->user_data == NULL) return PJ_FALSE;
  instance = (ls200_pjsip_instance *)parameters->user_data;
  (void)memset(&challenge, 0, sizeof(challenge));
  (void)memset(&credential, 0, sizeof(credential));
  (void)memset(realm, 0, sizeof(realm));
  (void)memset(nonce, 0, sizeof(nonce));
  (void)memset(opaque, 0, sizeof(opaque));
  (void)memset(algorithm, 0, sizeof(algorithm));
  (void)memset(qop, 0, sizeof(qop));
  if (!pjsip_registration_can_retry(instance, parameters, &challenge, realm, nonce,
                                    opaque, algorithm, qop))
    return pjsip_abandon_registration_challenge(session, token, instance, parameters,
                                                secret_data, sizeof(secret_data));
  status = pjsip_registration_prepare_retry(session, instance, parameters, &challenge,
                                            realm, &secret, &credential, &retry);
  pjsip_clear_registration_credentials(session, &credential);
  pjsip_wipe(secret_data, sizeof(secret_data));
  if (status == PJ_SUCCESS && retry != NULL) {
    (void)pjsip_auth_clt_async_send_req(session, token, retry);
    return PJ_TRUE;
  }
  return pjsip_abandon_registration_challenge(session, token, instance, parameters,
                                              secret_data, sizeof(secret_data));
}

static void pjsip_on_registration_complete(struct pjsip_regc_cbparam *parameters) {
  ls200_pjsip_instance *instance;
  if (parameters == NULL || parameters->token == NULL) return;
  instance = (ls200_pjsip_instance *)parameters->token;
  instance->registration_status_code = parameters->code;
  instance->registration_active = parameters->status == PJ_SUCCESS &&
      parameters->code >= 200 && parameters->code < 300 &&
      parameters->expiration != 0U;
  instance->registration_expiration = instance->registration_active != 0
      ? parameters->expiration : 0U;
  if (instance->registration_active != 0)
    instance->registration_digest_attempts = 0U;
}

static ls200_status pjsip_initialize_registration(
    ls200_pjsip_instance *instance, const ls200_pjsip_context *settings,
    const ls200_sip_endpoint_config *endpoint, pjsip_transport *transport) {
  pjsip_auth_clt_async_setting authentication;
  pjsip_tpselector selector;
  pjsip_tx_data *request = NULL;
  pj_str_t registrar = pj_str((char *)settings->registrar_uri);
  pj_status_t status;
  status = pjsip_regc_create(instance->endpoint, instance,
                             &pjsip_on_registration_complete,
                             &instance->registration);
  if (status != PJ_SUCCESS) return pjsip_status(status);
  status = pjsip_regc_init(instance->registration, &registrar,
                           &instance->local_uri, &instance->local_uri, 1,
                           &instance->local_uri, 300U);
  (void)memset(&selector, 0, sizeof(selector));
  selector.type = PJSIP_TPSELECTOR_TRANSPORT;
  selector.u.transport = transport;
  if (status == PJ_SUCCESS)
    status = pjsip_regc_set_transport(instance->registration, &selector);
  (void)memset(&authentication, 0, sizeof(authentication));
  authentication.cb = &pjsip_on_registration_challenge;
  authentication.user_data = instance;
  if (status == PJ_SUCCESS)
    status = pjsip_auth_clt_async_configure(
        pjsip_regc_get_auth_sess(instance->registration), &authentication);
  if (status == PJ_SUCCESS)
    status = pjsip_regc_register(instance->registration, PJ_TRUE, &request);
  if (status == PJ_SUCCESS) status = pjsip_regc_send(instance->registration, request);
  if (status != PJ_SUCCESS) {
    pjsip_regc_destroy(instance->registration);
    instance->registration = NULL;
  }
  (void)endpoint;
  return pjsip_status(status);
}

static ls200_status pjsip_register_proxy(ls200_pjsip_instance *instance,
                                         const ls200_pjsip_context *settings,
                                         const ls200_sip_endpoint_config *endpoint) {
  ls200_sip_resolver_request request;
  ls200_sip_resolver_result result;
  pjsip_transport *transport = NULL;
  pj_sockaddr remote;
  int remote_length;
  ls200_status status;
  if (settings->profile != LS200_ZOOM_PROFILE_PROXY_REGISTRATION)
    return LS200_STATUS_OK;
  if (instance == NULL || settings->registrar_uri[0] == '\0' || endpoint == NULL ||
      endpoint->credential_provider == NULL || endpoint->auth_username == NULL ||
      endpoint->auth_username[0] == '\0') return LS200_STATUS_CONFIGURATION_ERROR;
  (void)memset(&request, 0, sizeof(request));
  (void)memset(&result, 0, sizeof(result));
  request.target_uri = settings->registrar_uri;
  request.preferred_transport = endpoint->preferred_transport;
  request.enable_tls = endpoint->enable_tls;
  status = pjsip_driver_resolve(NULL, instance, &request, &result);
  if (status == LS200_STATUS_OK)
    status = pjsip_reauthorize_transport(instance, &result.peer);
  if (status == LS200_STATUS_OK)
    status = pjsip_peer_to_sockaddr(&result.peer, &remote, &remote_length);
  if (status == LS200_STATUS_OK &&
      pjsip_acquire_authorized_transport(instance, settings->registrar_uri,
                                         &result.peer, &remote, remote_length,
                                         &transport) != PJ_SUCCESS)
    status = LS200_STATUS_IO_ERROR;
  if (status != LS200_STATUS_OK) return status;
  instance->registration_credential_provider = endpoint->credential_provider;
  instance->registration_credential_context = endpoint->credential_context;
  instance->registration_digest_limit = endpoint->max_digest_retries == 0U
      ? 2U : endpoint->max_digest_retries;
  instance->registration_enabled = 1;
  status = pjsip_initialize_registration(instance, settings, endpoint, transport);
  pjsip_transport_dec_ref(transport);
  return status;
}
