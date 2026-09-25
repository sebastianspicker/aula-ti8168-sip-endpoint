#ifndef LS200_MEDIA_SESSION_PRIVATE_H
#define LS200_MEDIA_SESSION_PRIVATE_H

#include "ls200_sipd/media_session.h"
#include "ls200_sipd/h264.h"
#include "../sdp/negotiated_private.h"
#include "rx_playout.h"

#define LS200_MEDIA_SESSION_VIDEO_PACE_NS UINT64_C(1000000)
#define LS200_MEDIA_SESSION_AUDIO_PACE_NS UINT64_C(20000000)
#define LS200_MEDIA_SESSION_MAX_CODECS (LS200_SDP_MAX_SELECTED_CODECS * 2U)
#define LS200_MEDIA_SESSION_FEEDBACK_MINIMUM_INTERVAL_MS 250U
#define LS200_MEDIA_SESSION_RX_REORDER_HOLD_NS UINT64_C(20000000)

typedef struct ls200_media_session_stream {
  ls200_rtp_session *transport;
  ls200_rtp_send_queue *queue;
  /* RFC 4733 shares the audio transport but has an independent bounded
   * scheduler so an audio mute cannot discard or stall a telephone event. */
  ls200_rtp_send_queue *dtmf_queue;
  ls200_rtcp_reporter *reporter;
  ls200_rtp_identity identity;
  uint8_t payload_types[LS200_SDP_MAX_SELECTED_CODECS];
  ls200_rx_payload_clock payload_clocks[LS200_SDP_MAX_SELECTED_CODECS];
  size_t payload_type_count;
  uint32_t receive_media_id;
  uint64_t sent_packets;
  uint64_t sent_octets;
  uint64_t send_drop_packets;
  uint32_t received_ssrc;
  ls200_rx_playout playout;
} ls200_media_session_stream;

struct ls200_media_session {
  ls200_media_session_config config;
  ls200_media_session_state state;
  ls200_sdp_direction video_direction;
  ls200_sdp_direction audio_direction;
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
  ls200_media_session_stream video;
  ls200_media_session_stream audio;
  ls200_rx_shim *receive_shim;
  ls200_h264_parameter_sets parameter_sets;
  ls200_h264_parameter_sets receive_parameter_sets;
  ls200_aec *aec;
  ls200_h264_packetizer h264_packetizer;
  ls200_h264_depacketizer *h264_depacketizer;
  uint8_t *receive_video;
  size_t receive_video_capacity;
  uint8_t *receive_video_render;
  size_t receive_video_render_capacity;
  uint8_t *receive_audio;
  size_t receive_audio_capacity;
  ls200_g711_packetizer g711_packetizer;
  ls200_dtmf_sender_state *dtmf_sender;
  ls200_dtmf_receiver *dtmf_receiver;
  uint16_t audio_sequence_number;
  uint32_t dtmf_active_timestamp;
  uint32_t dtmf_next_timestamp;
  int dtmf_timestamp_valid;
  uint64_t last_dtmf_request_ns;
  uint64_t last_receive_pli_ns;
  uint8_t audio_remainder[LS200_G711_SAMPLES_PER_PACKET];
  size_t audio_remainder_length;
  uint8_t aec_near_remainder[LS200_AEC_FRAME_BYTES];
  size_t aec_near_remainder_length;
  uint8_t aec_reference_remainder[LS200_AEC_FRAME_BYTES];
  size_t aec_reference_remainder_length;
  uint64_t received_packets;
  uint64_t rejected_packets;
  uint64_t drained_bytes;
  uint64_t video_access_unit_drops;
  int receive_video_waiting_for_idr;
  ls200_media_session_status status;
};

int ls200_media_session_direction_sends(ls200_sdp_direction direction);
void ls200_media_session_record_error(ls200_media_session *session, ls200_status status);
void ls200_media_session_record_activity(ls200_media_session *session);
ls200_status ls200_media_session_request_keyframe_internal(
    ls200_media_session *session);
ls200_status ls200_media_session_collect_payloads(
    const ls200_sdp_codec *codecs, size_t codec_count,
    ls200_media_session_stream *stream);
ls200_status ls200_media_session_collect_negotiated_payloads(
    const ls200_sdp_negotiated_codec *codecs, size_t codec_count,
    ls200_media_session_stream *stream);
void ls200_media_session_release_prepared(ls200_media_session *session, int stop_backend);
int ls200_media_session_h264_profile_matches(const ls200_media_session *session);
ls200_status ls200_media_session_send_receive_bye(ls200_media_session *session);
ls200_status ls200_media_session_pump_video_internal(ls200_media_session *session,
                                                      ls200_deadline deadline);
ls200_status ls200_media_session_pump_audio_internal(ls200_media_session *session,
                                                      ls200_deadline deadline);
ls200_status ls200_media_session_flush_stream_internal(
    ls200_media_session *session, ls200_media_session_stream *stream);
ls200_status ls200_media_session_drain_stream_internal(
    ls200_media_session *session, ls200_media_session_stream *stream);
ls200_status ls200_media_session_playout_stream_internal(
    ls200_media_session *session, ls200_media_session_stream *stream);
ls200_status ls200_media_session_emit_receive_report_internal(
    ls200_media_session *session, ls200_media_session_stream *stream);
ls200_status ls200_media_session_reserve_internal(
    ls200_media_session *session, const ls200_sdp_local_capabilities *requested,
    ls200_sdp_local_capabilities *out_capabilities);
ls200_status ls200_media_session_prepare_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_session *negotiated);
void ls200_media_session_prepare_finalize_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_negotiated_media *audio);
ls200_status ls200_media_session_prepare_select_codecs_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_media *video,
    const ls200_sdp_receive_media *receive_video,
    const ls200_sdp_negotiated_media *audio,
    const ls200_sdp_receive_media *receive_audio);
ls200_status ls200_media_session_select_dtmf_internal(
    const ls200_sdp_negotiated_media *media,
    const ls200_sdp_receive_media *receive_media,
    uint8_t *out_payload_type, uint8_t *out_receive_payload_type,
    int *out_negotiated);
ls200_status ls200_media_session_poll_internal(ls200_media_session *session,
                                               ls200_deadline deadline);
uint64_t ls200_media_session_next_action_ns(const ls200_media_session *session,
                                             uint64_t now_ns);
ls200_status ls200_media_session_send_dtmf_internal(ls200_media_session *session,
                                                    uint8_t digit,
                                                    uint16_t duration_samples,
                                                    int end);
ls200_status ls200_media_session_commit_internal(ls200_media_session *session);
ls200_status ls200_media_session_set_video_transmit_enabled_internal(
    ls200_media_session *session, int enabled);
ls200_status ls200_media_session_set_audio_muted_internal(
    ls200_media_session *session, int muted);
void ls200_media_session_rollback_internal(ls200_media_session *session);
ls200_status ls200_media_session_stop_internal(ls200_media_session *session);
ls200_status ls200_media_session_get_status_internal(const ls200_media_session *session,
                                                      ls200_media_session_status *out_status);
typedef struct ls200_media_metrics {
  uint64_t video_queue_depth;
  uint64_t audio_queue_depth;
  uint64_t video_packet_drops;
  uint64_t audio_packet_drops;
  uint64_t video_access_unit_drops;
  uint64_t backend_drops;
  uint64_t backend_restarts;
} ls200_media_metrics;
ls200_status ls200_media_session_get_metrics_internal(
    const ls200_media_session *session, ls200_media_metrics *out_metrics);
size_t ls200_media_session_get_descriptors_internal(
    const ls200_media_session *session, int *descriptors, size_t capacity);
void ls200_media_session_destroy_internal(ls200_media_session *session);

void ls200_media_session_report_rtcp_internal(
    const ls200_media_session *session, const ls200_media_session_stream *stream,
    ls200_bytes packet);

ls200_status ls200_rx_shim_accept_rtcp_source_internal(
    ls200_rx_shim *shim, uint32_t media_id, const ls200_rtp_source *source,
    ls200_bytes packet, ls200_rtcp_compound_summary *summary);
ls200_status ls200_media_session_accept_feedback_internal(
    ls200_media_session *session, ls200_media_session_stream *stream,
    uint32_t sender_ssrc, ls200_bytes packet);
ls200_status ls200_media_session_flush_repairs_internal(ls200_media_session *session);
ls200_status ls200_media_session_prepare_feedback_internal(
    ls200_media_session *session, const ls200_sdp_negotiated_session *negotiated);

#endif
