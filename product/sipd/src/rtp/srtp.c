#include "retransmit_private.h"
#include "session_internal.h"

#include <limits.h>
#include <pthread.h>
#include <string.h>

#if LS200_SIPD_HAVE_SRTP
#include <srtp2/srtp.h>
#endif

ls200_status ls200_rtp_srtp_usage_check(
    const ls200_rtp_srtp_direction_usage *usage) {
  if (usage == NULL || usage->rtp_limit == 0U || usage->rtcp_limit == 0U)
    return LS200_STATUS_INVALID_ARGUMENT;
  return usage->rtp_packets < usage->rtp_limit &&
                 usage->rtcp_packets < usage->rtcp_limit ?
             LS200_STATUS_OK : LS200_STATUS_LIMIT_EXCEEDED;
}

void ls200_rtp_srtp_usage_success(ls200_rtp_srtp_direction_usage *usage,
                                  int rtcp) {
  if (usage == NULL) return;
  if (rtcp != 0) ++usage->rtcp_packets;
  else ++usage->rtp_packets;
}

#if LS200_SIPD_HAVE_SRTP
static void ls200_rtp_srtp_secure_zero(void *memory, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)memory;
  while (length > 0U) {
    *cursor = 0U;
    ++cursor;
    --length;
  }
}

static pthread_once_t ls200_rtp_srtp_once = PTHREAD_ONCE_INIT;
static srtp_err_status_t ls200_rtp_srtp_init_status = srtp_err_status_init_fail;

static void ls200_rtp_srtp_initialize_once(void) {
  ls200_rtp_srtp_init_status = srtp_init();
}

static ls200_status ls200_rtp_srtp_initialize(void) {
  return pthread_once(&ls200_rtp_srtp_once, ls200_rtp_srtp_initialize_once) == 0 &&
                 ls200_rtp_srtp_init_status == srtp_err_status_ok ?
             LS200_STATUS_OK : LS200_STATUS_INTERNAL_ERROR;
}

static ls200_status ls200_rtp_srtp_create(void **out_context, uint32_t ssrc,
                                           srtp_ssrc_type_t ssrc_type,
                                           const uint8_t key_salt[LS200_RTP_SRTP_MASTER_KEY_SALT_BYTES]) {
  srtp_policy_t policy;
  uint8_t key[LS200_RTP_SRTP_MASTER_KEY_SALT_BYTES];
  srtp_t context = NULL;
  srtp_err_status_t result;
  ls200_status status;
  if (out_context == NULL || *out_context != NULL || key_salt == NULL) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  status = ls200_rtp_srtp_initialize();
  if (status != LS200_STATUS_OK) return status;
  (void)memset(&policy, 0, sizeof(policy));
  (void)memcpy(key, key_salt, sizeof(key));
  srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
  srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
  policy.ssrc.type = ssrc_type;
  policy.ssrc.value = ssrc;
  policy.key = key;
  policy.next = NULL;
  result = srtp_create(&context, &policy);
  status = result == srtp_err_status_ok ? LS200_STATUS_OK : LS200_STATUS_INTERNAL_ERROR;
  ls200_rtp_srtp_secure_zero(key, sizeof(key));
  ls200_rtp_srtp_secure_zero(&policy, sizeof(policy));
  if (status != LS200_STATUS_OK) {
    if (context != NULL) (void)srtp_dealloc(context);
    return status;
  }
  *out_context = context;
  return LS200_STATUS_OK;
}

static int ls200_rtp_srtp_packet_is_valid(const void *context,
                                          const ls200_mutable_bytes *packet) {
  return context != NULL && packet != NULL && packet->data != NULL &&
         packet->length <= (size_t)INT_MAX &&
         packet->capacity <= (size_t)INT_MAX &&
         packet->length <= packet->capacity;
}

static srtp_err_status_t ls200_rtp_srtp_apply(void *context,
                                               uint8_t *packet, int *length,
                                               int outbound, int rtcp) {
  if (outbound != 0) {
    return rtcp != 0 ? srtp_protect_rtcp((srtp_t)context, packet, length) :
                       srtp_protect((srtp_t)context, packet, length);
  }
  return rtcp != 0 ? srtp_unprotect_rtcp((srtp_t)context, packet, length) :
                     srtp_unprotect((srtp_t)context, packet, length);
}

static ls200_status ls200_rtp_srtp_process(void *context,
                                            ls200_mutable_bytes *packet,
                                            int outbound, int rtcp) {
  size_t overhead;
  int length;
  srtp_err_status_t result;
  if (!ls200_rtp_srtp_packet_is_valid(context, packet)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  overhead = rtcp != 0 ? LS200_RTP_SRTP_RTCP_OVERHEAD_BYTES :
                         LS200_RTP_SRTP_RTP_OVERHEAD_BYTES;
  if (outbound != 0) {
    if (packet->capacity < overhead || packet->length > packet->capacity - overhead) {
      return LS200_STATUS_LIMIT_EXCEEDED;
    }
  } else if (packet->length < overhead) {
    return LS200_STATUS_SECURITY_ERROR;
  }
  length = (int)packet->length;
  result = ls200_rtp_srtp_apply(context, packet->data, &length, outbound, rtcp);
  if (result != srtp_err_status_ok || length < 0 || (size_t)length > packet->capacity) {
    return outbound != 0 ? LS200_STATUS_INTERNAL_ERROR : LS200_STATUS_SECURITY_ERROR;
  }
  packet->length = (size_t)length;
  return LS200_STATUS_OK;
}
#endif

#if LS200_SIPD_HAVE_SRTP
static int ls200_rtp_srtp_configuration_is_valid(
    const ls200_rtp_session *session, const ls200_rtp_srtp_config *config,
    const ls200_rtp_srtp_limits *limits) {
  return session != NULL && config != NULL && limits != NULL &&
      session->remote_target_set && session->srtp_enabled == 0 &&
      session->srtp_outbound == NULL && session->srtp_inbound == NULL &&
      limits->local_outbound > 0U &&
      limits->local_outbound <= LS200_RTP_SRTP_MAX_RTP_LIFETIME &&
      limits->remote_inbound > 0U &&
      limits->remote_inbound <= LS200_RTP_SRTP_MAX_RTP_LIFETIME;
}

static void ls200_rtp_srtp_set_usage(
    ls200_rtp_srtp_direction_usage *usage, uint64_t lifetime) {
  usage->rtp_limit = lifetime;
  usage->rtcp_limit = lifetime < LS200_RTP_SRTP_MAX_RTCP_LIFETIME ?
                          lifetime : LS200_RTP_SRTP_MAX_RTCP_LIFETIME;
}
#endif

ls200_status ls200_rtp_srtp_configure(ls200_rtp_session *session,
                                      const ls200_rtp_srtp_config *config,
                                      const ls200_rtp_srtp_limits *limits) {
#if LS200_SIPD_HAVE_SRTP
  ls200_status status;
  if (!ls200_rtp_srtp_configuration_is_valid(session, config, limits)) {
    return LS200_STATUS_STATE_ERROR;
  }
  ls200_rtp_retransmit_reset(session);
  status = ls200_rtp_srtp_create(&session->srtp_outbound, session->identity.ssrc,
                                 ssrc_specific, config->local_outbound_key_salt);
  if (status == LS200_STATUS_OK) {
    status = ls200_rtp_srtp_create(&session->srtp_inbound, 0U, ssrc_any_inbound,
                                   config->remote_inbound_key_salt);
  }
  if (status != LS200_STATUS_OK) {
    ls200_rtp_srtp_destroy(session);
    return status;
  }
  ls200_rtp_srtp_set_usage(&session->srtp_outbound_usage,
                            limits->local_outbound);
  ls200_rtp_srtp_set_usage(&session->srtp_inbound_usage,
                            limits->remote_inbound);
  session->srtp_enabled = 1;
  return LS200_STATUS_OK;
#else
  (void)session;
  (void)config;
  (void)limits;
  return LS200_STATUS_UNSUPPORTED;
#endif
}

#if LS200_SIPD_HAVE_SRTP
static ls200_status ls200_rtp_srtp_process_limited(
    void *context, ls200_mutable_bytes *packet, int outbound, int rtcp,
    ls200_rtp_srtp_direction_usage *usage) {
  ls200_status status = ls200_rtp_srtp_usage_check(usage);
  if (status != LS200_STATUS_OK) return status;
  status = ls200_rtp_srtp_process(context, packet, outbound, rtcp);
  if (status == LS200_STATUS_OK) ls200_rtp_srtp_usage_success(usage, rtcp);
  return status;
}
#endif

ls200_status ls200_rtp_srtp_protect_rtp(ls200_rtp_session *session,
                                        ls200_mutable_bytes *packet) {
#if LS200_SIPD_HAVE_SRTP
  if (session == NULL || session->srtp_enabled == 0) return LS200_STATUS_OK;
  return ls200_rtp_srtp_process_limited(session->srtp_outbound, packet, 1, 0,
                                        &session->srtp_outbound_usage);
#else
  (void)session;
  (void)packet;
  return LS200_STATUS_OK;
#endif
}

ls200_status ls200_rtp_srtp_unprotect_rtp(ls200_rtp_session *session,
                                          ls200_mutable_bytes *packet) {
#if LS200_SIPD_HAVE_SRTP
  if (session == NULL || session->srtp_enabled == 0) return LS200_STATUS_OK;
  return ls200_rtp_srtp_process_limited(session->srtp_inbound, packet, 0, 0,
                                        &session->srtp_inbound_usage);
#else
  (void)session;
  (void)packet;
  return LS200_STATUS_OK;
#endif
}

ls200_status ls200_rtp_srtp_protect_rtcp(ls200_rtp_session *session,
                                         ls200_mutable_bytes *packet) {
#if LS200_SIPD_HAVE_SRTP
  if (session == NULL || session->srtp_enabled == 0) return LS200_STATUS_OK;
  return ls200_rtp_srtp_process_limited(session->srtp_outbound, packet, 1, 1,
                                        &session->srtp_outbound_usage);
#else
  (void)session;
  (void)packet;
  return LS200_STATUS_OK;
#endif
}

ls200_status ls200_rtp_srtp_unprotect_rtcp(ls200_rtp_session *session,
                                           ls200_mutable_bytes *packet) {
#if LS200_SIPD_HAVE_SRTP
  if (session == NULL || session->srtp_enabled == 0) return LS200_STATUS_OK;
  return ls200_rtp_srtp_process_limited(session->srtp_inbound, packet, 0, 1,
                                        &session->srtp_inbound_usage);
#else
  (void)session;
  (void)packet;
  return LS200_STATUS_OK;
#endif
}

void ls200_rtp_srtp_destroy(ls200_rtp_session *session) {
  ls200_rtp_retransmit_reset(session);
#if LS200_SIPD_HAVE_SRTP
  if (session == NULL) return;
  if (session->srtp_inbound != NULL) (void)srtp_dealloc((srtp_t)session->srtp_inbound);
  if (session->srtp_outbound != NULL) (void)srtp_dealloc((srtp_t)session->srtp_outbound);
  session->srtp_inbound = NULL;
  session->srtp_outbound = NULL;
  session->srtp_enabled = 0;
  (void)memset(&session->srtp_outbound_usage, 0,
               sizeof(session->srtp_outbound_usage));
  (void)memset(&session->srtp_inbound_usage, 0,
               sizeof(session->srtp_inbound_usage));
#else
  (void)session;
#endif
}
