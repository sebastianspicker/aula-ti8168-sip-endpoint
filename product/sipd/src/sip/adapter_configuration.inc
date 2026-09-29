static int adapter_limits_are_valid(const aula_sip_endpoint_config *config) {
  return config->limits.maximum_message_bytes <=
             AULA_SIPD_MAX_SIP_MESSAGE_BYTES &&
         config->limits.maximum_sdp_bytes <= AULA_SIPD_MAX_SDP_BYTES &&
         config->limits.maximum_header_count <= 256U &&
         config->limits.maximum_transaction_count <= 64U &&
         config->limits.maximum_dialog_count <= 8U &&
         config->limits.maximum_digest_challenges_per_dialog <=
             AULA_SIP_MAX_DIGEST_RETRIES;
}

static int adapter_profile_is_valid(aula_zoom_profile profile) {
  return profile >= AULA_ZOOM_PROFILE_DIRECT_CRC &&
         profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB;
}

static int adapter_tcp_fallback_is_valid(const aula_sip_endpoint_config *config) {
  return config->allow_tcp_fallback == 0 ||
      (config->profile == AULA_ZOOM_PROFILE_PRIVATE_LAB &&
       config->preferred_transport == AULA_TRANSPORT_UDP &&
       config->enable_tls == 0);
}

static int adapter_proxy_transport_is_valid(const aula_sip_endpoint_config *config) {
  return config->profile != AULA_ZOOM_PROFILE_PROXY_REGISTRATION ||
      (config->enable_tls != 0 && config->preferred_transport == AULA_TRANSPORT_TLS);
}

static int adapter_retry_limits_are_valid(const aula_sip_endpoint_config *config) {
  return config->max_retransmissions <= AULA_SIP_MAX_RETRANSMISSIONS &&
         config->transaction_timeout_ms <= AULA_SIP_MAX_TIMEOUT_MS &&
         config->max_digest_retries <= AULA_SIP_MAX_DIGEST_RETRIES;
}

static int adapter_config_is_valid(const aula_sip_endpoint_config *config) {
  aula_sip_dial_target dial_target;
  if (config == NULL) return 0;
  if (!adapter_profile_is_valid(config->profile)) return 0;
  if (aula_sip_parse_dial_target(config->outbound_uri, &dial_target) !=
      AULA_STATUS_OK)
    return 0;
  if (config->auth_username != NULL &&
      !aula_sip_text_is_safe(config->auth_username,
                              AULA_SIP_MAX_USERNAME_BYTES))
    return 0;
  return adapter_proxy_transport_is_valid(config) &&
         adapter_tcp_fallback_is_valid(config) &&
         adapter_retry_limits_are_valid(config) &&
         config->preferred_transport <= AULA_TRANSPORT_TLS &&
         adapter_limits_are_valid(config);
}

static int adapter_policy_arguments_are_valid(const aula_sip_adapter *adapter,
                                              aula_zoom_profile profile,
                                              int enable_tls,
                                              aula_transport transport) {
  return adapter != NULL && (enable_tls == 0 || enable_tls == 1) &&
         transport <= AULA_TRANSPORT_TLS && adapter_profile_is_valid(profile);
}

static int adapter_policy_transport_is_valid(aula_zoom_profile profile,
                                             int enable_tls,
                                             aula_transport transport) {
  return profile == AULA_ZOOM_PROFILE_PRIVATE_LAB ||
      (enable_tls != 0 && transport == AULA_TRANSPORT_TLS);
}

static int adapter_policy_has_proxy_credentials(const aula_sip_adapter *adapter,
                                                aula_zoom_profile profile) {
  return profile != AULA_ZOOM_PROFILE_PROXY_REGISTRATION ||
      (adapter->endpoint.auth_username != NULL &&
       adapter->endpoint.auth_secret_file != NULL);
}

static int adapter_policy_keeps_tcp_fallback(aula_zoom_profile profile,
                                             int enable_tls,
                                             aula_transport transport) {
  return profile == AULA_ZOOM_PROFILE_PRIVATE_LAB &&
         enable_tls == 0 && transport == AULA_TRANSPORT_UDP;
}

aula_status aula_sip_adapter_apply_policy(
    aula_sip_adapter *adapter, aula_zoom_profile profile, int enable_tls,
    aula_transport preferred_transport) {
  if (!adapter_policy_arguments_are_valid(adapter, profile, enable_tls,
                                          preferred_transport)) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  if (adapter->state != AULA_SIP_ADAPTER_ACTIVE || adapter->dialogs != NULL)
    return AULA_STATUS_STATE_ERROR;
  if (!adapter_policy_transport_is_valid(profile, enable_tls, preferred_transport)) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  if (!adapter_policy_has_proxy_credentials(adapter, profile)) {
    return AULA_STATUS_PERMISSION_DENIED;
  }
  adapter->endpoint.profile = profile;
  adapter->endpoint.enable_tls = enable_tls;
  adapter->endpoint.preferred_transport = preferred_transport;
  if (!adapter_policy_keeps_tcp_fallback(profile, enable_tls, preferred_transport))
    adapter->allow_tcp_fallback = 0;
  return AULA_STATUS_OK;
}

aula_status aula_sip_adapter_apply_settings(
    aula_sip_adapter *adapter, aula_zoom_profile profile, int tls_verified) {
  if (tls_verified != 0 && tls_verified != 1) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  /* Both UI-safe TLS states retain the deployment-validated TLS transport;
   * only the daemon can truthfully report whether verification completed. */
  return aula_sip_adapter_apply_policy(adapter, profile, 1,
                                        AULA_TRANSPORT_TLS);
}

static void adapter_set_limit_defaults(aula_sip_adapter *adapter) {
  if (adapter->endpoint.limits.maximum_message_bytes == 0U)
    adapter->endpoint.limits.maximum_message_bytes = AULA_SIPD_MAX_SIP_MESSAGE_BYTES;
  if (adapter->endpoint.limits.maximum_sdp_bytes == 0U)
    adapter->endpoint.limits.maximum_sdp_bytes = AULA_SIPD_MAX_SDP_BYTES;
  if (adapter->endpoint.limits.maximum_header_count == 0U)
    adapter->endpoint.limits.maximum_header_count = 128U;
  if (adapter->endpoint.limits.maximum_transaction_count == 0U)
    adapter->endpoint.limits.maximum_transaction_count = 16U;
  if (adapter->endpoint.limits.maximum_dialog_count == 0U)
    adapter->endpoint.limits.maximum_dialog_count = 2U;
  if (adapter->endpoint.limits.maximum_digest_challenges_per_dialog == 0U)
    adapter->endpoint.limits.maximum_digest_challenges_per_dialog =
        adapter->max_digest_retries;
  if (adapter->max_digest_retries >
      adapter->endpoint.limits.maximum_digest_challenges_per_dialog)
    adapter->max_digest_retries =
        adapter->endpoint.limits.maximum_digest_challenges_per_dialog;
}
