#ifndef AULA_MEDIA_SESSION_PRIVATE_H
#define AULA_MEDIA_SESSION_PRIVATE_H

#include "aula_sipd/media_session.h"
#include "aula_sipd/h264.h"
#include "../sdp/negotiated_private.h"
#include "rx_playout.h"

#define AULA_MEDIA_SESSION_VIDEO_PACE_NS UINT64_C(1000000)
#define AULA_MEDIA_SESSION_AUDIO_PACE_NS UINT64_C(20000000)
#define AULA_MEDIA_SESSION_MAX_CODECS (AULA_SDP_MAX_SELECTED_CODECS * 2U)
#define AULA_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS 250U
#define AULA_MEDIA_SESSION_RX_REORDER_HOLD_NS UINT64_C(20000000)

typedef struct aula_media_session_stream {
  aula_rtp_session *transport;
  aula_rtp_send_queue *queue;
  /* RFC 4733 shares the audio transport but has an independent bounded
   * scheduler so an audio mute cannot discard or stall a telephone event. */
  aula_rtp_send_queue *dtmf_queue;
  aula_rtcp_reporter *reporter;
  aula_rtp_identity identity;
  uint8_t payload_types[AULA_SDP_MAX_SELECTED_CODECS];
  aula_rx_payload_clock payload_clocks[AULA_SDP_MAX_SELECTED_CODECS];
  size_t payload_type_count;
  uint32_t receive_media_id;
  uint64_t sent_packets;
  uint64_t sent_octets;
  uint64_t send_drop_packets;
  uint32_t received_ssrc;
  aula_rx_playout playout;
} aula_media_session_stream;

struct aula_media_session {
  aula_media_session_config config;
  aula_media_session_state state;
  aula_sdp_direction video_direction;
  aula_sdp_direction audio_direction;
  uint8_t video_payload_type;
  uint8_t audio_payload_type;
  uint8_t dtmf_payload_type;
  uint8_t video_receive_payload_type;
  uint8_t audio_receive_payload_type;
  uint8_t dtmf_receive_payload_type;
  int directional_payloads_configured;
  uint32_t video_feedback_mask;
  uint64_t last_video_feedback_ns;
  uint64_t video_feedback_requests;
  uint64_t video_retransmitted_packets;
  uint64_t requested_video_bitrate_bps;
  uint32_t last_fir_sender;
  uint8_t last_fir_sequence;
  int have_fir_sequence;
  int use_pcma;
  int dtmf_negotiated;
  int dtmf_enabled;
  char negotiated_h264_profile_level_id[7];
  int backend_open;
  int backend_started;
  int video_transmission_ready;
  int video_transmit_enabled;
  int audio_muted;
  aula_media_session_stream video;
  aula_media_session_stream audio;
  aula_rx_shim *receive_shim;
  aula_h264_parameter_sets parameter_sets;
  aula_h264_parameter_sets receive_parameter_sets;
  aula_aec *aec;
  aula_h264_packetizer h264_packetizer;
  aula_h264_depacketizer *h264_depacketizer;
  uint8_t *receive_video;
  size_t receive_video_capacity;
  uint8_t *receive_video_render;
  size_t receive_video_render_capacity;
  uint8_t *receive_audio;
  size_t receive_audio_capacity;
  aula_g711_packetizer g711_packetizer;
  aula_dtmf_sender_state *dtmf_sender;
  aula_dtmf_receiver *dtmf_receiver;
  uint16_t audio_sequence_number;
  uint32_t dtmf_active_timestamp;
  uint32_t dtmf_next_timestamp;
  int dtmf_timestamp_valid;
  uint64_t last_dtmf_request_ns;
  uint64_t last_receive_pli_ns;
  uint8_t audio_remainder[AULA_G711_SAMPLES_PER_PACKET];
  size_t audio_remainder_length;
  uint8_t aec_near_remainder[AULA_AEC_FRAME_BYTES];
  size_t aec_near_remainder_length;
  uint8_t aec_reference_remainder[AULA_AEC_FRAME_BYTES];
  size_t aec_reference_remainder_length;
  uint64_t received_packets;
  uint64_t rejected_packets;
  uint64_t drained_bytes;
  uint64_t video_access_unit_drops;
  int receive_video_waiting_for_idr;
  aula_media_session_status status;
};

int aula_media_session_direction_sends(aula_sdp_direction direction);
void aula_media_session_record_error(aula_media_session *session, aula_status status);
void aula_media_session_record_activity(aula_media_session *session);
aula_status aula_media_session_request_keyframe_internal(
    aula_media_session *session);
aula_status aula_media_session_collect_payloads(
    const aula_sdp_codec *codecs, size_t codec_count,
    aula_media_session_stream *stream);
aula_status aula_media_session_collect_negotiated_payloads(
    const aula_sdp_negotiated_codec *codecs, size_t codec_count,
    aula_media_session_stream *stream);
void aula_media_session_release_prepared(aula_media_session *session, int stop_backend);
int aula_media_session_h264_profile_matches(const aula_media_session *session);
aula_status aula_media_session_send_receive_bye(aula_media_session *session);
aula_status aula_media_session_pump_video_internal(aula_media_session *session,
                                                      aula_deadline deadline);
aula_status aula_media_session_pump_audio_internal(aula_media_session *session,
                                                      aula_deadline deadline);
aula_status aula_media_session_flush_stream_internal(
    aula_media_session *session, aula_media_session_stream *stream);
aula_status aula_media_session_drain_stream_internal(
    aula_media_session *session, aula_media_session_stream *stream);
aula_status aula_media_session_playout_stream_internal(
    aula_media_session *session, aula_media_session_stream *stream);
aula_status aula_media_session_emit_receive_report_internal(
    aula_media_session *session, aula_media_session_stream *stream);
aula_status aula_media_session_reserve_internal(
    aula_media_session *session, const aula_sdp_local_capabilities *requested,
    aula_sdp_local_capabilities *out_capabilities);
aula_status aula_media_session_prepare_internal(
    aula_media_session *session, const aula_sdp_negotiated_session *negotiated);
void aula_media_session_prepare_finalize_internal(
    aula_media_session *session, const aula_sdp_negotiated_media *video,
    const aula_sdp_negotiated_media *audio);
aula_status aula_media_session_prepare_select_codecs_internal(
    aula_media_session *session, const aula_sdp_negotiated_media *video,
    const aula_sdp_receive_media *receive_video,
    const aula_sdp_negotiated_media *audio,
    const aula_sdp_receive_media *receive_audio);
aula_status aula_media_session_select_dtmf_internal(
    const aula_sdp_negotiated_media *media,
    const aula_sdp_receive_media *receive_media,
    uint8_t *out_payload_type, uint8_t *out_receive_payload_type,
    int *out_negotiated);
aula_status aula_media_session_poll_internal(aula_media_session *session,
                                               aula_deadline deadline);
uint64_t aula_media_session_next_action_ns(const aula_media_session *session,
                                             uint64_t now_ns);
aula_status aula_media_session_send_dtmf_internal(aula_media_session *session,
                                                    uint8_t digit,
                                                    uint16_t duration_samples,
                                                    int end);
aula_status aula_media_session_commit_internal(aula_media_session *session);
aula_status aula_media_session_set_video_transmit_enabled_internal(
    aula_media_session *session, int enabled);
aula_status aula_media_session_set_audio_muted_internal(
    aula_media_session *session, int muted);
void aula_media_session_rollback_internal(aula_media_session *session);
aula_status aula_media_session_stop_internal(aula_media_session *session);
aula_status aula_media_session_get_status_internal(const aula_media_session *session,
                                                      aula_media_session_status *out_status);
typedef struct aula_media_metrics {
  uint64_t video_queue_depth;
  uint64_t audio_queue_depth;
  uint64_t video_packet_drops;
  uint64_t audio_packet_drops;
  uint64_t video_access_unit_drops;
  uint64_t backend_drops;
  uint64_t backend_restarts;
} aula_media_metrics;
aula_status aula_media_session_get_metrics_internal(
    const aula_media_session *session, aula_media_metrics *out_metrics);
size_t aula_media_session_get_descriptors_internal(
    const aula_media_session *session, int *descriptors, size_t capacity);
void aula_media_session_destroy_internal(aula_media_session *session);

void aula_media_session_report_rtcp_internal(
    const aula_media_session *session, const aula_media_session_stream *stream,
    aula_bytes packet);

aula_status aula_rx_shim_accept_rtcp_source_internal(
    aula_rx_shim *shim, uint32_t media_id, const aula_rtp_source *source,
    aula_bytes packet, aula_rtcp_compound_summary *summary);
aula_status aula_media_session_accept_feedback_internal(
    aula_media_session *session, aula_media_session_stream *stream,
    uint32_t sender_ssrc, aula_bytes packet);
aula_status aula_media_session_flush_repairs_internal(aula_media_session *session);
aula_status aula_media_session_prepare_feedback_internal(
    aula_media_session *session, const aula_sdp_negotiated_session *negotiated);

#endif
