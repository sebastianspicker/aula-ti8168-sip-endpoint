#ifndef LS200_SIPD_ENDPOINT_H
#define LS200_SIPD_ENDPOINT_H

#include "ls200_sipd/config.h"
#include "ls200_sipd/control.h"
#include "ls200_sipd/sdp.h"
#include "ls200_sipd/sip.h"
#include "ls200_sipd/zoom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_endpoint_options {
  /* A dependency binding or unit-test fake. Runtime inputs stay snapshot-owned. */
  const ls200_sip_driver_config *sip_driver;
  /* Secret access stays outside the adapter and is invoked only for a bounded,
   * validated digest challenge. */
  ls200_sip_credential_provider credential_provider;
  void *credential_context;
  /* Optional deployment/account media policy. If omitted, media targets must
   * exactly match the authorized SIP peer address. */
  ls200_sdp_media_target_authorizer media_target_authorizer;
  void *media_target_context;
  /* Optional opaque local renderer binding.  The caller owns its lifetime;
   * the endpoint reserves, starts, and releases it while deriving safe SDP
   * receive directions.  No vendor-specific renderer type crosses this API. */
  struct ls200_media_renderer *renderer;
} ls200_endpoint_options;

typedef struct ls200_endpoint ls200_endpoint;

ls200_status ls200_endpoint_create(const ls200_config *config,
                                   const ls200_endpoint_options *options,
                                   ls200_endpoint **out_endpoint);
/* Reserves retained RTP/RTCP ports before constructing the initial SDP offer. */
ls200_status ls200_endpoint_start(ls200_endpoint *endpoint);
/* Starts a call from typed Zoom fields. No caller-supplied SIP URI crosses
 * this boundary. Proxy and private-lab domains are taken from the approved
 * endpoint URI in the immutable configuration snapshot. */
ls200_status ls200_endpoint_originate(
    ls200_endpoint *endpoint, const ls200_zoom_dial_request *request);
/* Sends one bounded RFC 4733 event on an established media session. */
ls200_status ls200_endpoint_send_dtmf(ls200_endpoint *endpoint, uint8_t digit,
                                      uint16_t duration_samples, int end);
ls200_status ls200_endpoint_poll(ls200_endpoint *endpoint, ls200_deadline deadline);
ls200_status ls200_endpoint_command(ls200_endpoint *endpoint,
                                    ls200_control_command command);
ls200_status ls200_endpoint_get_status(const ls200_endpoint *endpoint,
                                       ls200_endpoint_status *out_status);
void ls200_endpoint_destroy(ls200_endpoint *endpoint);

#ifdef __cplusplus
}
#endif

#endif
