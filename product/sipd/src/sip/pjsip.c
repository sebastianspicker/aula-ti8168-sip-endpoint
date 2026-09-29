#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "aula_sipd/pjsip.h"

#include "adapter_internal.h"
#include "crc_route.h"
#include "aula_sipd/platform.h"
#include "pjsip_readiness.h"
#include "resolver_internal.h"

#include <string.h>

#define AULA_PJSIP_REGISTRAR_BYTES 512U
#define AULA_PJSIP_TLS_CA_BYTES 512U
#define AULA_PJSIP_DIGEST_FIELD_BYTES 256U

typedef struct aula_pjsip_context {
  aula_zoom_profile profile;
  char registrar_uri[AULA_PJSIP_REGISTRAR_BYTES];
  char local_uri[AULA_PJSIP_REGISTRAR_BYTES];
  char tls_ca_file[AULA_PJSIP_TLS_CA_BYTES];
  char crc_address[16];
} aula_pjsip_context;

static aula_pjsip_context pjsip_context;

static int pjsip_profile_is_valid(aula_zoom_profile profile) {
  return profile >= AULA_ZOOM_PROFILE_DIRECT_CRC &&
         profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB;
}

static int pjsip_registrar_is_safe(const char *value) {
  aula_sip_dial_target target;
  return value != NULL && strstr(value, "http") == NULL &&
         aula_sip_parse_dial_target(value, &target) == AULA_STATUS_OK;
}

/* Keep the private native adapter in independently reviewable units while
 * retaining one translation unit: no PJLIB/PJSIP object crosses sip.h. */
#if defined(AULA_SIPD_HAVE_PJSIP) && AULA_SIPD_HAVE_PJSIP
#include "pjsip_callbacks.inc"
#include "pjsip_transport.inc"
#include "pjsip_actions.inc"
#include "pjsip_events.inc"
#endif

#if !defined(AULA_SIPD_HAVE_PJSIP) || !AULA_SIPD_HAVE_PJSIP
aula_status aula_pjsip_media_readiness_sync(const int *descriptors,
                                               size_t descriptor_count) {
  (void)descriptors;
  (void)descriptor_count;
  return AULA_STATUS_UNSUPPORTED;
}

void aula_pjsip_media_readiness_cancel(void) {}
#endif

static aula_status pjsip_validate_proxy_options(
    const aula_pjsip_driver_options *options) {
  if (options->profile != AULA_ZOOM_PROFILE_PROXY_REGISTRATION)
    return options->registrar_uri == NULL ? AULA_STATUS_OK :
                                            AULA_STATUS_INVALID_ARGUMENT;
  if (!pjsip_registrar_is_safe(options->registrar_uri) ||
      strlen(options->registrar_uri) >= sizeof(pjsip_context.registrar_uri))
    return AULA_STATUS_INVALID_DATA;
  return AULA_STATUS_OK;
}

static aula_status pjsip_validate_text_option(const char *value,
                                               size_t capacity,
                                               int requires_sip_uri) {
  if (value == NULL) return AULA_STATUS_OK;
  if (strlen(value) >= capacity) return AULA_STATUS_INVALID_DATA;
  if (requires_sip_uri != 0 &&
      aula_sip_parse_dial_target(value, &(aula_sip_dial_target){0}) !=
          AULA_STATUS_OK)
    return AULA_STATUS_INVALID_DATA;
  return AULA_STATUS_OK;
}

static aula_status pjsip_validate_options(
    const aula_pjsip_driver_options *options) {
  aula_status status;
  if (options == NULL || !pjsip_profile_is_valid(options->profile))
    return AULA_STATUS_INVALID_ARGUMENT;
  status = pjsip_validate_proxy_options(options);
  if (status != AULA_STATUS_OK) return status;
  status = pjsip_validate_text_option(options->local_uri,
                                      sizeof(pjsip_context.local_uri), 1);
  if (status != AULA_STATUS_OK) return status;
  if (options->tls_ca_file != NULL && options->tls_ca_file[0] == '\0')
    return AULA_STATUS_INVALID_DATA;
  return pjsip_validate_text_option(options->tls_ca_file,
                                    sizeof(pjsip_context.tls_ca_file), 0);
}

static void pjsip_copy_option(char *destination, const char *value) {
  if (value != NULL)
    (void)memcpy(destination, value, strlen(value) + 1U);
}

static void pjsip_configure_context(const aula_pjsip_driver_options *options) {
  (void)memset(&pjsip_context, 0, sizeof(pjsip_context));
  pjsip_context.profile = options->profile;
  pjsip_copy_option(pjsip_context.registrar_uri, options->registrar_uri);
  pjsip_copy_option(pjsip_context.local_uri, options->local_uri);
  pjsip_copy_option(pjsip_context.tls_ca_file, options->tls_ca_file);
}

aula_status aula_pjsip_driver_configure_route(
    const aula_pjsip_driver_options *options, const char *crc_address,
    aula_sip_driver_config *out_config) {
  aula_status status;
  if (out_config == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  status = pjsip_validate_options(options);
  if (status != AULA_STATUS_OK) return status;
#if defined(AULA_SIPD_HAVE_PJSIP) && AULA_SIPD_HAVE_PJSIP
  if (pjsip_active_instance != NULL) return AULA_STATUS_STATE_ERROR;
#endif
  if (!aula_sip_crc_address_is_valid(crc_address))
    return AULA_STATUS_INVALID_DATA;
  if (crc_address != NULL && crc_address[0] != '\0' &&
      (options->profile != AULA_ZOOM_PROFILE_DIRECT_CRC ||
       options->tls_ca_file == NULL))
    return AULA_STATUS_CONFIGURATION_ERROR;
  pjsip_configure_context(options);
  pjsip_copy_option(pjsip_context.crc_address, crc_address);
  (void)memset(out_config, 0, sizeof(*out_config));
  out_config->selected_library = "pjproject";
  out_config->selected_source_revision = AULA_PJSIP_SOURCE_REVISION;
  out_config->context = &pjsip_context;
#if defined(AULA_SIPD_HAVE_PJSIP) && AULA_SIPD_HAVE_PJSIP
  out_config->vtable = &pjsip_vtable;
#endif
  return AULA_STATUS_OK;
}

aula_status aula_pjsip_driver_configure(
    const aula_pjsip_driver_options *options,
    aula_sip_driver_config *out_config) {
  return aula_pjsip_driver_configure_route(options, NULL, out_config);
}

aula_status aula_pjsip_driver_create(
    const aula_pjsip_driver_options *options, aula_sip_driver **out_driver) {
  aula_sip_driver_config config;
  aula_status status;
  if (out_driver == NULL || *out_driver != NULL) return AULA_STATUS_INVALID_ARGUMENT;
  status = aula_pjsip_driver_configure(options, &config);
  if (status != AULA_STATUS_OK) return status;
  return aula_sip_driver_create(&config, out_driver);
}

int aula_pjsip_driver_is_available(void) {
#if defined(AULA_SIPD_HAVE_PJSIP) && AULA_SIPD_HAVE_PJSIP
  return 1;
#else
  return 0;
#endif
}
