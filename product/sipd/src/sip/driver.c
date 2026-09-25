#include "ls200_sipd/sip.h"

#include "driver_internal.h"

#include <stdlib.h>
#include <string.h>

#define LS200_SIP_DRIVER_NAME_BYTES 96U
#define LS200_SIP_DRIVER_REVISION_BYTES 128U

struct ls200_sip_driver {
  char library[LS200_SIP_DRIVER_NAME_BYTES];
  char revision[LS200_SIP_DRIVER_REVISION_BYTES];
  void *implementation_context;
  const ls200_sip_driver_vtable *vtable;
  void *instance;
  int bound;
};

static void ls200_sip_driver_wipe(void *memory, size_t length) {
  volatile unsigned char *bytes = (volatile unsigned char *)memory;
  while (length-- > 0U) *bytes++ = 0U;
}

static int ls200_sip_driver_text_is_safe(const char *value, size_t capacity) {
  size_t index;
  if (value == NULL || value[0] == '\0' || strlen(value) >= capacity) return 0;
  for (index = 0U; value[index] != '\0'; ++index) {
    unsigned char c = (unsigned char)value[index];
    if (c < 0x21U || c > 0x7eU) return 0;
  }
  return 1;
}

static int ls200_sip_driver_vtable_has_lifecycle(const ls200_sip_driver_vtable *vtable) {
  return vtable->create != NULL && vtable->destroy != NULL &&
         vtable->resolve != NULL && vtable->start_invite != NULL &&
         vtable->poll != NULL;
}

static int ls200_sip_driver_vtable_has_requests(const ls200_sip_driver_vtable *vtable) {
  return vtable->send_ack != NULL && vtable->send_cancel != NULL &&
         vtable->send_bye != NULL && vtable->start_reinvite != NULL &&
         vtable->answer_reinvite != NULL && vtable->reject_reinvite != NULL;
}

static int ls200_sip_driver_vtable_has_dialog_ops(const ls200_sip_driver_vtable *vtable) {
  return vtable->submit_digest != NULL && vtable->abort_dialog != NULL &&
         vtable->destroy_dialog != NULL;
}

static int ls200_sip_driver_vtable_is_complete(const ls200_sip_driver_vtable *vtable) {
  return vtable != NULL && vtable->version == LS200_SIP_DRIVER_VTABLE_VERSION &&
         vtable->struct_size >= sizeof(*vtable) &&
         ls200_sip_driver_vtable_has_lifecycle(vtable) &&
         ls200_sip_driver_vtable_has_requests(vtable) &&
         ls200_sip_driver_vtable_has_dialog_ops(vtable);
}

ls200_status ls200_sip_driver_create(const ls200_sip_driver_config *config,
                                     ls200_sip_driver **out_driver) {
  ls200_sip_driver *driver;
  if (config == NULL || out_driver == NULL || *out_driver != NULL ||
      !ls200_sip_driver_text_is_safe(config->selected_library, LS200_SIP_DRIVER_NAME_BYTES) ||
      !ls200_sip_driver_text_is_safe(config->selected_source_revision,
                                     LS200_SIP_DRIVER_REVISION_BYTES)) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  driver = (ls200_sip_driver *)calloc(1U, sizeof(*driver));
  if (driver == NULL) return LS200_STATUS_INTERNAL_ERROR;
  (void)memcpy(driver->library, config->selected_library, strlen(config->selected_library) + 1U);
  (void)memcpy(driver->revision, config->selected_source_revision,
               strlen(config->selected_source_revision) + 1U);
  driver->implementation_context = config->context;
  /* A missing or incomplete table is deliberately a non-operational boundary. */
  if (ls200_sip_driver_vtable_is_complete(config->vtable)) driver->vtable = config->vtable;
  *out_driver = driver;
  return LS200_STATUS_OK;
}

int ls200_sip_driver_is_operational(const ls200_sip_driver *driver) {
  return driver != NULL && driver->vtable != NULL && driver->bound != 0;
}

ls200_status ls200_sip_driver_bind(ls200_sip_driver *driver,
                                   const ls200_sip_endpoint_config *endpoint,
                                   ls200_sip_driver_event_sink sink, void *sink_context) {
  ls200_status status;
  if (driver == NULL || endpoint == NULL || sink == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (driver->vtable == NULL) return LS200_STATUS_UNSUPPORTED;
  if (driver->bound != 0) return LS200_STATUS_STATE_ERROR;
  status = driver->vtable->create(driver->implementation_context, endpoint, sink,
                                  sink_context, &driver->instance);
  if (status != LS200_STATUS_OK) {
    driver->instance = NULL;
    return status;
  }
  if (driver->instance == NULL) return LS200_STATUS_INTERNAL_ERROR;
  driver->bound = 1;
  return LS200_STATUS_OK;
}

ls200_status ls200_sip_driver_resolve(ls200_sip_driver *driver,
                                      const ls200_sip_resolver_request *request,
                                      ls200_sip_resolver_result *result) {
  if (!ls200_sip_driver_is_operational(driver)) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->resolve(driver->implementation_context, driver->instance,
                                 request, result);
}

ls200_status ls200_sip_driver_start_invite(ls200_sip_driver *driver,
                                           const ls200_sip_invite_request *request,
                                           void **out_dialog) {
  if (!ls200_sip_driver_is_operational(driver)) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->start_invite(driver->implementation_context, driver->instance,
                                      request, out_dialog);
}

ls200_status ls200_sip_driver_poll(ls200_sip_driver *driver, ls200_deadline deadline) {
  if (!ls200_sip_driver_is_operational(driver)) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->poll(driver->implementation_context, driver->instance, deadline);
}

ls200_status ls200_sip_driver_get_registration_status(
    const ls200_sip_driver *driver,
    ls200_sip_registration_status *out_status) {
  if (out_status == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_status, 0, sizeof(*out_status));
  if (!ls200_sip_driver_is_operational(driver)) return LS200_STATUS_UNSUPPORTED;
  if (driver->vtable->get_registration_status == NULL) return LS200_STATUS_OK;
  return driver->vtable->get_registration_status(driver->implementation_context,
                                                   driver->instance, out_status);
}

ls200_status ls200_sip_driver_send_ack(ls200_sip_driver *driver, void *dialog,
                                       const ls200_sip_transaction_identity *transaction) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL || transaction == NULL ||
      transaction->handle == NULL || transaction->role != LS200_SIP_TRANSACTION_ROLE_CLIENT ||
      transaction->method != LS200_SIP_METHOD_INVITE || transaction->cseq_number == 0U) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->send_ack(driver->implementation_context, driver->instance, dialog, transaction);
}

ls200_status ls200_sip_driver_send_cancel(ls200_sip_driver *driver, void *dialog, uint32_t cseq) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->send_cancel(driver->implementation_context, driver->instance, dialog, cseq);
}

ls200_status ls200_sip_driver_send_bye(ls200_sip_driver *driver, void *dialog, uint32_t cseq) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->send_bye(driver->implementation_context, driver->instance, dialog, cseq);
}

ls200_status ls200_sip_driver_start_reinvite(ls200_sip_driver *driver, void *dialog,
                                              const ls200_sip_reinvite_request *request) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->start_reinvite(driver->implementation_context, driver->instance,
                                        dialog, request);
}

ls200_status ls200_sip_driver_answer_reinvite(ls200_sip_driver *driver, void *dialog,
                                               ls200_bytes answer,
                                               const ls200_sip_transaction_identity *transaction) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL || transaction == NULL ||
      transaction->handle == NULL || transaction->role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
      transaction->method != LS200_SIP_METHOD_REINVITE || transaction->cseq_number == 0U) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->answer_reinvite(driver->implementation_context, driver->instance,
                                         dialog, answer, transaction);
}

ls200_status ls200_sip_driver_reject_reinvite(ls200_sip_driver *driver, void *dialog,
                                               uint16_t status_code,
                                               const ls200_sip_transaction_identity *transaction) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL || transaction == NULL ||
      transaction->handle == NULL || transaction->role != LS200_SIP_TRANSACTION_ROLE_SERVER ||
      transaction->method != LS200_SIP_METHOD_REINVITE || transaction->cseq_number == 0U) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->reject_reinvite(driver->implementation_context, driver->instance,
                                         dialog, status_code, transaction);
}

ls200_status ls200_sip_driver_submit_digest(ls200_sip_driver *driver, void *dialog,
                                            const ls200_sip_digest_response *response) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->submit_digest(driver->implementation_context, driver->instance,
                                       dialog, response);
}

ls200_status ls200_sip_driver_abort_dialog(ls200_sip_driver *driver, void *dialog) {
  if (!ls200_sip_driver_is_operational(driver) || dialog == NULL) return LS200_STATUS_UNSUPPORTED;
  return driver->vtable->abort_dialog(driver->implementation_context, driver->instance, dialog);
}

void ls200_sip_driver_destroy_dialog(ls200_sip_driver *driver, void *dialog) {
  if (ls200_sip_driver_is_operational(driver) && dialog != NULL) {
    driver->vtable->destroy_dialog(driver->implementation_context, driver->instance, dialog);
  }
}

void ls200_sip_driver_destroy(ls200_sip_driver *driver) {
  if (driver != NULL) {
    if (driver->bound != 0 && driver->vtable != NULL && driver->vtable->destroy != NULL) {
      driver->vtable->destroy(driver->implementation_context, driver->instance);
    }
    ls200_sip_driver_wipe(driver, sizeof(*driver));
    free(driver);
  }
}
