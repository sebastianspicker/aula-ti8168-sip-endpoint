#ifndef LS200_SIPD_ENDPOINT_INTERNAL_H
#define LS200_SIPD_ENDPOINT_INTERNAL_H

#include "ls200_sipd/backend.h"
#include "ls200_sipd/endpoint.h"
#include "ls200_sipd/log.h"
#include "ls200_sipd/media_session.h"

struct ls200_endpoint {
  ls200_call_config *snapshot;
  const ls200_config_view *view;
  ls200_backend_config backend_config;
  ls200_sip_endpoint_config sip_config;
  ls200_media_address_options addresses;
  ls200_logger *logger;
  ls200_call *call;
  ls200_sip_adapter *adapter;
  ls200_sip_driver *driver;
  ls200_media_renderer *renderer;
  ls200_sip_dialog *dialog;
  ls200_media_session *media;
  ls200_sdp_session *offer;
  ls200_sdp_local_capabilities reserved;
  ls200_media_session *pending_media;
  ls200_sdp_local_capabilities pending_reserved;
  ls200_sdp_negotiated_session *staged_reinvite;
  ls200_control_server *control;
  ls200_endpoint_status status;
  uint32_t settings_revision;
  int settings_media_managed;
  uint64_t event_revision;
  ls200_call_state event_call_state;
  int event_state_initialized;
  ls200_status last_endpoint_error;
  uint8_t bind_address[4];
  ls200_sip_transport_info authorized_sip_peer;
  ls200_sdp_media_target_authorizer media_target_authorizer;
  void *media_target_context;
  int has_authorized_sip_peer;
  uint64_t invite_deadline_ns;
  uint64_t retry_deadline_ns;
  uint64_t shutdown_deadline_ns;
  uint64_t poll_late_count;
  uint64_t poll_max_lateness_ns;
  char source_h264_profile_level_id[7];
  int source_h264_profile_present;
  int started;
  int shutdown_requested;
  int reinvite_pending;
  uint32_t reinvite_cseq;
  char approved_target_domain[256];
  uint16_t approved_target_port;
  uint64_t dtmf_next_emit_ns;
  uint16_t dtmf_duration_samples;
  uint8_t dtmf_digit;
  uint8_t dtmf_end_packets_remaining;
  uint8_t layout_next_digits_pending;
};

void endpoint_log(ls200_endpoint *endpoint, ls200_log_level level,
                  const char *event, const char *reason);
void endpoint_refresh_status(ls200_endpoint *endpoint);
const char *endpoint_call_state_name(ls200_call_state state);
ls200_status endpoint_scope(void *context, const ls200_sip_transport_info *peer,
                            int fallback);
ls200_status endpoint_create_adapter(ls200_endpoint *endpoint,
                                     const ls200_endpoint_options *options);
/* Runtime settings are loaded before the adapter is created.  Control
 * mutations publish to the in-memory adapter first, roll that change back on
 * every pre-rename persistence failure, and treat rename as the durable commit
 * point before publishing the endpoint-visible revision. */
ls200_status endpoint_settings_load(ls200_endpoint *endpoint);
ls200_status endpoint_settings_validate_candidate(
    const ls200_endpoint *endpoint, ls200_zoom_profile profile,
    int media_managed);
ls200_status endpoint_settings_persist(const ls200_endpoint *endpoint,
                                       uint32_t revision,
                                       ls200_zoom_profile profile,
                                       int media_managed);
#if defined(LS200_SIPD_TEST_FAULTS)
void endpoint_settings_test_fail_next_parent_sync(void);
void endpoint_settings_test_fail_next_rename(void);
#endif
ls200_status endpoint_control_settings(ls200_endpoint *endpoint,
                                       ls200_bytes payload,
                                       ls200_mutable_bytes *response);
ls200_status endpoint_create_control(ls200_endpoint *endpoint);
ls200_status endpoint_begin_invite(ls200_endpoint *endpoint, int initial);
void endpoint_finish_shutdown(ls200_endpoint *endpoint);
void endpoint_take_stop_signal(ls200_endpoint *endpoint);
void endpoint_poll_dtmf(ls200_endpoint *endpoint, uint64_t now);
void endpoint_clear_dtmf_schedule(ls200_endpoint *endpoint);
int endpoint_shutdown_due(const ls200_endpoint *endpoint, uint64_t now);
int endpoint_diagnostics_request_is_valid(ls200_bytes payload);
ls200_status endpoint_write_diagnostics(ls200_endpoint *endpoint,
                                        ls200_mutable_bytes *response);
int endpoint_event_snapshot_request_is_valid(ls200_bytes payload);
ls200_status endpoint_write_event_snapshot(ls200_endpoint *endpoint,
                                           ls200_mutable_bytes *response);
ls200_status endpoint_write_metrics(ls200_endpoint *endpoint,
                                    ls200_mutable_bytes *response);
void endpoint_poll_media(ls200_endpoint *endpoint, ls200_deadline deadline);
ls200_deadline endpoint_effective_poll_deadline(const ls200_endpoint *endpoint,
                                                uint64_t now,
                                                ls200_deadline requested);
uint64_t endpoint_deadline_after(uint64_t now, uint64_t delay_ns);
ls200_status endpoint_create_media_session(ls200_endpoint *endpoint,
                                           ls200_media_session **out_media);
void endpoint_destroy_media_session(ls200_media_session **session);
void endpoint_release_pending_media(ls200_endpoint *endpoint);
void endpoint_report_media_totals(ls200_media_session *session);
void endpoint_handle_picture_fast_update(void *context);
void endpoint_release_media(ls200_endpoint *endpoint);
void endpoint_retire_dialog(ls200_endpoint *endpoint);
const ls200_sdp_codec *endpoint_video_codecs(
    const ls200_endpoint *endpoint, ls200_sdp_codec *dynamic_codec,
    char *dynamic_fmtp, size_t dynamic_fmtp_capacity, size_t *out_count);
ls200_status endpoint_discover_source_h264(ls200_endpoint *endpoint);
ls200_status endpoint_prepare_offer(ls200_endpoint *endpoint,
                                    ls200_mutable_bytes *serialized);
void endpoint_retry(ls200_endpoint *endpoint, uint32_t retry_after_seconds);
void endpoint_media_failure(ls200_endpoint *endpoint);
ls200_status endpoint_stage_reinvite(ls200_endpoint *endpoint,
                                     const ls200_sip_event *event);
void endpoint_sip_event(void *context, const ls200_sip_event *event);

#endif
