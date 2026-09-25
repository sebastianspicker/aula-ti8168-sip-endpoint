#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "ls200_sipd/pjsip.h"

#include "adapter_internal.h"
#include "crc_route.h"
#include "ls200_sipd/platform.h"
#include "pjsip_readiness.h"
#include "resolver_internal.h"

#include <string.h>

#define LS200_PJSIP_REGISTRAR_BYTES 512U
#define LS200_PJSIP_TLS_CA_BYTES 512U
#define LS200_PJSIP_DIGEST_FIELD_BYTES 256U

typedef struct ls200_pjsip_context {
  ls200_zoom_profile profile;
  char registrar_uri[LS200_PJSIP_REGISTRAR_BYTES];
  char local_uri[LS200_PJSIP_REGISTRAR_BYTES];
  char tls_ca_file[LS200_PJSIP_TLS_CA_BYTES];
  char crc_address[16];
} ls200_pjsip_context;

static ls200_pjsip_context pjsip_context;

static int pjsip_profile_is_valid(ls200_zoom_profile profile) {
  return profile >= LS200_ZOOM_PROFILE_DIRECT_CRC &&
         profile <= LS200_ZOOM_PROFILE_PRIVATE_LAB;
}

static int pjsip_registrar_is_safe(const char *value) {
  ls200_sip_dial_target target;
  return value != NULL && strstr(value, "http") == NULL &&
         ls200_sip_parse_dial_target(value, &target) == LS200_STATUS_OK;
}

/* Keep the private native adapter in independently reviewable units while
 * retaining one translation unit: no PJLIB/PJSIP object crosses sip.h. */
#if defined(LS200_SIPD_HAVE_PJSIP) && LS200_SIPD_HAVE_PJSIP
#include "pjsip_callbacks.inc"
#include "pjsip_transport.inc"
#include "pjsip_actions.inc"
#include "pjsip_events.inc"
#endif

#if !defined(LS200_SIPD_HAVE_PJSIP) || !LS200_SIPD_HAVE_PJSIP
ls200_status ls200_pjsip_media_readiness_sync(const int *descriptors,
                                               size_t descriptor_count) {
  (void)descriptors;
  (void)descriptor_count;
  return LS200_STATUS_UNSUPPORTED;
}

void ls200_pjsip_media_readiness_cancel(void) {}
#endif

static ls200_status pjsip_validate_proxy_options(
    const ls200_pjsip_driver_options *options) {
  if (options->profile != LS200_ZOOM_PROFILE_PROXY_REGISTRATION)
    return options->registrar_uri == NULL ? LS200_STATUS_OK :
                                            LS200_STATUS_INVALID_ARGUMENT;
  if (!pjsip_registrar_is_safe(options->registrar_uri) ||
      strlen(options->registrar_uri) >= sizeof(pjsip_context.registrar_uri))
    return LS200_STATUS_INVALID_DATA;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_validate_text_option(const char *value,
                                               size_t capacity,
                                               int requires_sip_uri) {
  if (value == NULL) return LS200_STATUS_OK;
  if (strlen(value) >= capacity) return LS200_STATUS_INVALID_DATA;
  if (requires_sip_uri != 0 &&
      ls200_sip_parse_dial_target(value, &(ls200_sip_dial_target){0}) !=
          LS200_STATUS_OK)
    return LS200_STATUS_INVALID_DATA;
  return LS200_STATUS_OK;
}

static ls200_status pjsip_validate_options(
    const ls200_pjsip_driver_options *options) {
  ls200_status status;
  if (options == NULL || !pjsip_profile_is_valid(options->profile))
    return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_validate_proxy_options(options);
  if (status != LS200_STATUS_OK) return status;
  status = pjsip_validate_text_option(options->local_uri,
                                      sizeof(pjsip_context.local_uri), 1);
  if (status != LS200_STATUS_OK) return status;
  if (options->tls_ca_file != NULL && options->tls_ca_file[0] == '\0')
    return LS200_STATUS_INVALID_DATA;
  return pjsip_validate_text_option(options->tls_ca_file,
                                    sizeof(pjsip_context.tls_ca_file), 0);
}

static void pjsip_copy_option(char *destination, const char *value) {
  if (value != NULL)
    (void)memcpy(destination, value, strlen(value) + 1U);
}

static void pjsip_configure_context(const ls200_pjsip_driver_options *options) {
  (void)memset(&pjsip_context, 0, sizeof(pjsip_context));
  pjsip_context.profile = options->profile;
  pjsip_copy_option(pjsip_context.registrar_uri, options->registrar_uri);
  pjsip_copy_option(pjsip_context.local_uri, options->local_uri);
  pjsip_copy_option(pjsip_context.tls_ca_file, options->tls_ca_file);
}

ls200_status ls200_pjsip_driver_configure_route(
    const ls200_pjsip_driver_options *options, const char *crc_address,
    ls200_sip_driver_config *out_config) {
  ls200_status status;
  if (out_config == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = pjsip_validate_options(options);
  if (status != LS200_STATUS_OK) return status;
#if defined(LS200_SIPD_HAVE_PJSIP) && LS200_SIPD_HAVE_PJSIP
  if (pjsip_active_instance != NULL) return LS200_STATUS_STATE_ERROR;
#endif
  if (!ls200_sip_crc_address_is_valid(crc_address))
    return LS200_STATUS_INVALID_DATA;
  if (crc_address != NULL && crc_address[0] != '\0' &&
      (options->profile != LS200_ZOOM_PROFILE_DIRECT_CRC ||
       options->tls_ca_file == NULL))
    return LS200_STATUS_CONFIGURATION_ERROR;
  pjsip_configure_context(options);
  pjsip_copy_option(pjsip_context.crc_address, crc_address);
  (void)memset(out_config, 0, sizeof(*out_config));
  out_config->selected_library = "pjproject";
  out_config->selected_source_revision = LS200_PJSIP_SOURCE_REVISION;
  out_config->context = &pjsip_context;
#if defined(LS200_SIPD_HAVE_PJSIP) && LS200_SIPD_HAVE_PJSIP
  out_config->vtable = &pjsip_vtable;
#endif
  return LS200_STATUS_OK;
}

ls200_status ls200_pjsip_driver_configure(
    const ls200_pjsip_driver_options *options,
    ls200_sip_driver_config *out_config) {
  return ls200_pjsip_driver_configure_route(options, NULL, out_config);
}

ls200_status ls200_pjsip_driver_create(
    const ls200_pjsip_driver_options *options, ls200_sip_driver **out_driver) {
  ls200_sip_driver_config config;
  ls200_status status;
  if (out_driver == NULL || *out_driver != NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = ls200_pjsip_driver_configure(options, &config);
  if (status != LS200_STATUS_OK) return status;
  return ls200_sip_driver_create(&config, out_driver);
}

int ls200_pjsip_driver_is_available(void) {
#if defined(LS200_SIPD_HAVE_PJSIP) && LS200_SIPD_HAVE_PJSIP
  return 1;
#else
  return 0;
#endif
}
