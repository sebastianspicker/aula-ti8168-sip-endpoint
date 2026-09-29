#ifndef AULA_SIPD_DRIVER_INTERNAL_H
#define AULA_SIPD_DRIVER_INTERNAL_H

#include "aula_sipd/sip.h"

int aula_sip_driver_is_operational(const aula_sip_driver *driver);
aula_status aula_sip_driver_bind(aula_sip_driver *driver,
                                   const aula_sip_endpoint_config *endpoint,
                                   aula_sip_driver_event_sink sink, void *sink_context);
aula_status aula_sip_driver_resolve(aula_sip_driver *driver,
                                      const aula_sip_resolver_request *request,
                                      aula_sip_resolver_result *result);
aula_status aula_sip_driver_start_invite(aula_sip_driver *driver,
                                           const aula_sip_invite_request *request,
                                           void **out_dialog);
aula_status aula_sip_driver_poll(aula_sip_driver *driver, aula_deadline deadline);
aula_status aula_sip_driver_get_registration_status(
    const aula_sip_driver *driver,
    aula_sip_registration_status *out_status);
aula_status aula_sip_driver_send_ack(aula_sip_driver *driver, void *dialog,
                                       const aula_sip_transaction_identity *transaction);
aula_status aula_sip_driver_send_cancel(aula_sip_driver *driver, void *dialog,
                                          uint32_t cseq);
aula_status aula_sip_driver_send_bye(aula_sip_driver *driver, void *dialog,
                                       uint32_t cseq);
aula_status aula_sip_driver_start_reinvite(aula_sip_driver *driver, void *dialog,
                                              const aula_sip_reinvite_request *request);
aula_status aula_sip_driver_answer_reinvite(
    aula_sip_driver *driver, void *dialog, aula_bytes answer,
    const aula_sip_transaction_identity *transaction);
aula_status aula_sip_driver_reject_reinvite(
    aula_sip_driver *driver, void *dialog, uint16_t status_code,
    const aula_sip_transaction_identity *transaction);
aula_status aula_sip_driver_submit_digest(aula_sip_driver *driver, void *dialog,
                                            const aula_sip_digest_response *response);
aula_status aula_sip_driver_abort_dialog(aula_sip_driver *driver, void *dialog);
void aula_sip_driver_destroy_dialog(aula_sip_driver *driver, void *dialog);

#endif
