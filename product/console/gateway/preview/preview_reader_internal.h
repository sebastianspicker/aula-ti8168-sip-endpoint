#ifndef AULA_CONSOLE_PREVIEW_READER_INTERNAL_H
#define AULA_CONSOLE_PREVIEW_READER_INTERNAL_H

#include "preview_reader.h"
#include "aula_sipd/h264.h"

struct aula_preview_reader {
  aula_preview_reader_config config;
  aula_preview_reader_state state;
  aula_rtsp_stream_parser *parser;
  aula_h264_depacketizer *depacketizer;
  aula_h264_parameter_sets parameter_sets;
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

int aula_preview_output_is_valid(const aula_mutable_bytes *output);
void aula_preview_clear_outputs(aula_mutable_bytes *request,
                                 aula_preview_access_unit *unit);
void aula_preview_secure_zero(void *value, size_t length);

aula_status aula_preview_render_request(
    aula_preview_reader *reader, const char *method, const char *uri,
    const char *extra_headers, aula_preview_reader_state next_state,
    aula_mutable_bytes *output);
aula_status aula_preview_consume_response(
    aula_preview_reader *reader, const aula_rtsp_message *message,
    aula_mutable_bytes *out_request);
aula_status aula_preview_consume_rtp(
    aula_preview_reader *reader, aula_bytes bytes,
    aula_preview_access_unit *out_unit);

#endif
