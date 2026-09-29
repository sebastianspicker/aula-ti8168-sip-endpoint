#ifndef AULA_SIPD_ENDPOINT_H
#define AULA_SIPD_ENDPOINT_H

#include "aula_sipd/config.h"
#include "aula_sipd/control.h"
#include "aula_sipd/sdp.h"
#include "aula_sipd/sip.h"
#include "aula_sipd/zoom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_endpoint_options {
  /* A dependency binding or unit-test fake. Runtime inputs stay snapshot-owned. */
  const aula_sip_driver_config *sip_driver;
  /* Secret access stays outside the adapter and is invoked only for a bounded,
   * validated digest challenge. */
  aula_sip_credential_provider credential_provider;
  void *credential_context;
  /* Optional deployment/account media policy. If omitted, media targets must
   * exactly match the authorized SIP peer address. */
  aula_sdp_media_target_authorizer media_target_authorizer;
  void *media_target_context;
  /* Optional opaque local renderer binding.  The caller owns its lifetime;
   * the endpoint reserves, starts, and releases it while deriving safe SDP
   * receive directions.  No vendor-specific renderer type crosses this API. */
  struct aula_media_renderer *renderer;
} aula_endpoint_options;

typedef struct aula_endpoint aula_endpoint;

aula_status aula_endpoint_create(const aula_config *config,
                                   const aula_endpoint_options *options,
                                   aula_endpoint **out_endpoint);
/* Reserves retained RTP/RTCP ports before constructing the initial SDP offer. */
aula_status aula_endpoint_start(aula_endpoint *endpoint);
/* Starts a call from typed Zoom fields. No caller-supplied SIP URI crosses
 * this boundary. Proxy and private-lab domains are taken from the approved
 * endpoint URI in the immutable configuration snapshot. */
aula_status aula_endpoint_originate(
    aula_endpoint *endpoint, const aula_zoom_dial_request *request);
/* Sends one bounded RFC 4733 event on an established media session. */
aula_status aula_endpoint_send_dtmf(aula_endpoint *endpoint, uint8_t digit,
                                      uint16_t duration_samples, int end);
aula_status aula_endpoint_poll(aula_endpoint *endpoint, aula_deadline deadline);
aula_status aula_endpoint_command(aula_endpoint *endpoint,
                                    aula_control_command command);
aula_status aula_endpoint_get_status(const aula_endpoint *endpoint,
                                       aula_endpoint_status *out_status);
void aula_endpoint_destroy(aula_endpoint *endpoint);

#ifdef __cplusplus
}
#endif

#endif
