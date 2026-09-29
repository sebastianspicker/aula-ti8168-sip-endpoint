#ifndef AULA_SIPD_ENDPOINT_INTERNAL_H
#define AULA_SIPD_ENDPOINT_INTERNAL_H

#include "aula_sipd/backend.h"
#include "aula_sipd/endpoint.h"
#include "aula_sipd/log.h"
#include "aula_sipd/media_session.h"

struct aula_endpoint {
  aula_call_config *snapshot;
  const aula_config_view *view;
  aula_backend_config backend_config;
  aula_sip_endpoint_config sip_config;
  aula_media_address_options addresses;
  aula_logger *logger;
  aula_call *call;
  aula_sip_adapter *adapter;
  aula_sip_driver *driver;
  aula_media_renderer *renderer;
  aula_sip_dialog *dialog;
  aula_media_session *media;
  aula_sdp_session *offer;
  aula_sdp_local_capabilities reserved;
  aula_media_session *pending_media;
  aula_sdp_local_capabilities pending_reserved;
  aula_sdp_negotiated_session *staged_reinvite;
  aula_control_server *control;
  aula_endpoint_status status;
  uint32_t settings_revision;
  int settings_media_managed;
  uint64_t event_revision;
  aula_call_state event_call_state;
  int event_state_initialized;
  aula_status last_endpoint_error;
  uint8_t bind_address[4];
  aula_sip_transport_info authorized_sip_peer;
  aula_sdp_media_target_authorizer media_target_authorizer;
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

void endpoint_log(aula_endpoint *endpoint, aula_log_level level,
                  const char *event, const char *reason);
void endpoint_refresh_status(aula_endpoint *endpoint);
const char *endpoint_call_state_name(aula_call_state state);
aula_status endpoint_scope(void *context, const aula_sip_transport_info *peer,
                            int fallback);
aula_status endpoint_create_adapter(aula_endpoint *endpoint,
                                     const aula_endpoint_options *options);
/* Runtime settings are loaded before the adapter is created.  Control
 * mutations publish to the in-memory adapter first, roll that change back on
 * every pre-rename persistence failure, and treat rename as the durable commit
 * point before publishing the endpoint-visible revision. */
aula_status endpoint_settings_load(aula_endpoint *endpoint);
aula_status endpoint_settings_validate_candidate(
    const aula_endpoint *endpoint, aula_zoom_profile profile,
    int media_managed);
aula_status endpoint_settings_persist(const aula_endpoint *endpoint,
                                       uint32_t revision,
                                       aula_zoom_profile profile,
                                       int media_managed);
aula_status endpoint_control_settings(aula_endpoint *endpoint,
                                       aula_bytes payload,
                                       aula_mutable_bytes *response);
aula_status endpoint_create_control(aula_endpoint *endpoint);
aula_status endpoint_begin_invite(aula_endpoint *endpoint, int initial);
void endpoint_finish_shutdown(aula_endpoint *endpoint);
void endpoint_take_stop_signal(aula_endpoint *endpoint);
void endpoint_poll_dtmf(aula_endpoint *endpoint, uint64_t now);
void endpoint_clear_dtmf_schedule(aula_endpoint *endpoint);
int endpoint_shutdown_due(const aula_endpoint *endpoint, uint64_t now);
int endpoint_diagnostics_request_is_valid(aula_bytes payload);
aula_status endpoint_write_diagnostics(aula_endpoint *endpoint,
                                        aula_mutable_bytes *response);
int endpoint_event_snapshot_request_is_valid(aula_bytes payload);
aula_status endpoint_write_event_snapshot(aula_endpoint *endpoint,
                                           aula_mutable_bytes *response);
aula_status endpoint_write_metrics(aula_endpoint *endpoint,
                                    aula_mutable_bytes *response);
void endpoint_poll_media(aula_endpoint *endpoint, aula_deadline deadline);
aula_deadline endpoint_effective_poll_deadline(const aula_endpoint *endpoint,
                                                uint64_t now,
                                                aula_deadline requested);
uint64_t endpoint_deadline_after(uint64_t now, uint64_t delay_ns);
aula_status endpoint_create_media_session(aula_endpoint *endpoint,
                                           aula_media_session **out_media);
void endpoint_destroy_media_session(aula_media_session **session);
void endpoint_release_pending_media(aula_endpoint *endpoint);
void endpoint_report_media_totals(aula_media_session *session);
void endpoint_handle_picture_fast_update(void *context);
void endpoint_release_media(aula_endpoint *endpoint);
void endpoint_retire_dialog(aula_endpoint *endpoint);
const aula_sdp_codec *endpoint_video_codecs(
    const aula_endpoint *endpoint, aula_sdp_codec *dynamic_codec,
    char *dynamic_fmtp, size_t dynamic_fmtp_capacity, size_t *out_count);
aula_status endpoint_discover_source_h264(aula_endpoint *endpoint);
aula_status endpoint_prepare_offer(aula_endpoint *endpoint,
                                    aula_mutable_bytes *serialized);
void endpoint_retry(aula_endpoint *endpoint, uint32_t retry_after_seconds);
void endpoint_media_failure(aula_endpoint *endpoint);
aula_status endpoint_stage_reinvite(aula_endpoint *endpoint,
                                     const aula_sip_event *event);
void endpoint_sip_event(void *context, const aula_sip_event *event);

#endif
