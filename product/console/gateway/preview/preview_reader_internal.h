#ifndef LS200_CONSOLE_PREVIEW_READER_INTERNAL_H
#define LS200_CONSOLE_PREVIEW_READER_INTERNAL_H

#include "preview_reader.h"
#include "ls200_sipd/h264.h"

struct ls200_preview_reader {
  ls200_preview_reader_config config;
  ls200_preview_reader_state state;
  ls200_rtsp_stream_parser *parser;
  ls200_h264_depacketizer *depacketizer;
  ls200_h264_parameter_sets parameter_sets;
  uint8_t *assembly;
  size_t assembly_length;
  char aggregate_uri[64];
  char setup_uri[192];
  uint32_t next_cseq;
  uint32_t parameter_set_generation;
  uint32_t locked_ssrc;
  uint16_t next_sequence;
  uint32_t current_timestamp;
  uint32_t last_timestamp;
  uint64_t current_timestamp_90khz;
  uint64_t last_timestamp_90khz;
  uint8_t payload_type;
  int ssrc_locked;
  int sequence_locked;
  int au_active;
  int timestamp_locked;
  int decoder_ready;
  int drop_until_marker;
  int ever_emitted;
  int discontinuity_pending;
};

int ls200_preview_output_is_valid(const ls200_mutable_bytes *output);
void ls200_preview_clear_outputs(ls200_mutable_bytes *request,
                                 ls200_preview_access_unit *unit);
void ls200_preview_secure_zero(void *value, size_t length);

ls200_status ls200_preview_render_request(
    ls200_preview_reader *reader, const char *method, const char *uri,
    const char *extra_headers, ls200_preview_reader_state next_state,
    ls200_mutable_bytes *output);
ls200_status ls200_preview_consume_response(
    ls200_preview_reader *reader, const ls200_rtsp_message *message,
    ls200_mutable_bytes *out_request);
ls200_status ls200_preview_consume_rtp(
    ls200_preview_reader *reader, ls200_bytes bytes,
    ls200_preview_access_unit *out_unit);

#endif
