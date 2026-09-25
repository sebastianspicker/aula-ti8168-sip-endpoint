#ifndef LS200_SIPD_DRIVER_INTERNAL_H
#define LS200_SIPD_DRIVER_INTERNAL_H

#include "ls200_sipd/sip.h"

int ls200_sip_driver_is_operational(const ls200_sip_driver *driver);
ls200_status ls200_sip_driver_bind(ls200_sip_driver *driver,
                                   const ls200_sip_endpoint_config *endpoint,
                                   ls200_sip_driver_event_sink sink, void *sink_context);
ls200_status ls200_sip_driver_resolve(ls200_sip_driver *driver,
                                      const ls200_sip_resolver_request *request,
                                      ls200_sip_resolver_result *result);
ls200_status ls200_sip_driver_start_invite(ls200_sip_driver *driver,
                                           const ls200_sip_invite_request *request,
                                           void **out_dialog);
ls200_status ls200_sip_driver_poll(ls200_sip_driver *driver, ls200_deadline deadline);
ls200_status ls200_sip_driver_get_registration_status(
    const ls200_sip_driver *driver,
    ls200_sip_registration_status *out_status);
ls200_status ls200_sip_driver_send_ack(ls200_sip_driver *driver, void *dialog,
                                       const ls200_sip_transaction_identity *transaction);
ls200_status ls200_sip_driver_send_cancel(ls200_sip_driver *driver, void *dialog,
                                          uint32_t cseq);
ls200_status ls200_sip_driver_send_bye(ls200_sip_driver *driver, void *dialog,
                                       uint32_t cseq);
ls200_status ls200_sip_driver_start_reinvite(ls200_sip_driver *driver, void *dialog,
                                              const ls200_sip_reinvite_request *request);
ls200_status ls200_sip_driver_answer_reinvite(
    ls200_sip_driver *driver, void *dialog, ls200_bytes answer,
    const ls200_sip_transaction_identity *transaction);
ls200_status ls200_sip_driver_reject_reinvite(
    ls200_sip_driver *driver, void *dialog, uint16_t status_code,
    const ls200_sip_transaction_identity *transaction);
ls200_status ls200_sip_driver_submit_digest(ls200_sip_driver *driver, void *dialog,
                                            const ls200_sip_digest_response *response);
ls200_status ls200_sip_driver_abort_dialog(ls200_sip_driver *driver, void *dialog);
void ls200_sip_driver_destroy_dialog(ls200_sip_driver *driver, void *dialog);

#endif
