#ifndef LS200_SIPD_PJSIP_H
#define LS200_SIPD_PJSIP_H

#include "ls200_sipd/sip.h"
#include "ls200_sipd/zoom.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_PJSIP_SOURCE_REVISION "b7317ffcaf1d20a38eb231cf15b7b65c4734dbb4"

/* The registrar is used only by the proxy-registration profile. The factory
 * copies it; it must be a SIP URI, never an HTTP(S) URL. */
typedef struct ls200_pjsip_driver_options {
  ls200_zoom_profile profile;
  const char *registrar_uri;
  /* Optional local SIP identity. If omitted, the endpoint outbound URI is
   * retained as the compatibility fallback when the driver is bound. */
  const char *local_uri;
  /* CA bundle used by the low-level TLS transport. It remains private to the
   * adapter and is never exposed through the portable SIP contract. */
  const char *tls_ca_file;
} ls200_pjsip_driver_options;

/* Validates and materializes the low-level backend configuration without
 * exposing PJLIB/PJSIP types. The resulting config remains valid until the
 * next configure call. */
ls200_status ls200_pjsip_driver_configure(
    const ls200_pjsip_driver_options *options,
    ls200_sip_driver_config *out_config);

/* Produces an opaque ls200_sip_driver. When PJSIP support was not selected at
 * configuration time the returned driver is metadata-only and follows the
 * existing UNSUPPORTED adapter path. */
ls200_status ls200_pjsip_driver_create(
    const ls200_pjsip_driver_options *options, ls200_sip_driver **out_driver);

int ls200_pjsip_driver_is_available(void);

#ifdef __cplusplus
}
#endif

#endif
