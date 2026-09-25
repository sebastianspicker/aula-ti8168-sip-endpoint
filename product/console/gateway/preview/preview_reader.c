#include "preview_reader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls200_preview_secure_zero(void *value, size_t length) {
  volatile uint8_t *cursor = (volatile uint8_t *)value;
  while (length-- != 0U) *cursor++ = 0U;
}

static int target_is_allowed(const uint8_t address[4]) {
  if (address[0] == 0U || address[0] == 255U) return 0;
  if (address[0] == 127U || address[0] == 10U) return 1;
  if (address[0] == 172U && address[1] >= 16U && address[1] <= 31U) return 1;
  if (address[0] == 192U && address[1] == 168U) return 1;
  return address[0] == 169U && address[1] == 254U;
}

int ls200_preview_output_is_valid(const ls200_mutable_bytes *output) {
  return output != NULL && (output->data != NULL || output->capacity == 0U);
}

void ls200_preview_clear_outputs(ls200_mutable_bytes *request,
                                 ls200_preview_access_unit *unit) {
  request->length = 0U;
  (void)memset(unit, 0, sizeof(*unit));
}

ls200_status ls200_preview_reader_create(
    const ls200_preview_reader_config *config,
    ls200_preview_reader **out_reader) {
  ls200_preview_reader *reader;
  ls200_status status;
  int allocation_failed;
  int written;
  if (config == NULL || out_reader == NULL || config->target_port == 0U ||
      !target_is_allowed(config->target_ipv4) ||
      config->maximum_access_unit_bytes < LS200_PREVIEW_MIN_ACCESS_UNIT_BYTES ||
      config->maximum_access_unit_bytes >
          LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES) {
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  *out_reader = NULL;
  reader = (ls200_preview_reader *)calloc(1U, sizeof(*reader));
  if (reader == NULL) return LS200_STATUS_INTERNAL_ERROR;
  reader->config = *config;
  written = snprintf(reader->aggregate_uri, sizeof(reader->aggregate_uri),
                     "rtsp://%u.%u.%u.%u:%u%s", config->target_ipv4[0],
                     config->target_ipv4[1], config->target_ipv4[2],
                     config->target_ipv4[3], config->target_port,
                     LS200_PREVIEW_MOVIE_PATH);
  if (written < 0 || (size_t)written >= sizeof(reader->aggregate_uri)) {
    free(reader);
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  reader->assembly = (uint8_t *)malloc(config->maximum_access_unit_bytes);
  status = ls200_rtsp_stream_parser_create(&reader->parser);
  if (status == LS200_STATUS_OK) {
    status = ls200_h264_depacketizer_create(config->maximum_access_unit_bytes,
                                            &reader->depacketizer);
  }
  allocation_failed = reader->assembly == NULL;
  if (allocation_failed || status != LS200_STATUS_OK) {
    ls200_h264_depacketizer_destroy(reader->depacketizer);
    ls200_rtsp_stream_parser_destroy(reader->parser);
    free(reader->assembly);
    ls200_preview_secure_zero(reader, sizeof(*reader));
    free(reader);
    return allocation_failed ? LS200_STATUS_INTERNAL_ERROR : status;
  }
  reader->state = LS200_PREVIEW_READER_NEW;
  *out_reader = reader;
  return LS200_STATUS_OK;
}

ls200_status ls200_preview_reader_start(ls200_preview_reader *reader,
                                        ls200_mutable_bytes *out_request) {
  if (reader == NULL || !ls200_preview_output_is_valid(out_request) ||
      reader->state != LS200_PREVIEW_READER_NEW) {
    return LS200_STATUS_STATE_ERROR;
  }
  out_request->length = 0U;
  reader->next_cseq = 1U;
  return ls200_preview_render_request(
      reader, "OPTIONS", reader->aggregate_uri, "",
      LS200_PREVIEW_READER_OPTIONS, out_request);
}

static ls200_status consume_message(ls200_preview_reader *reader,
                                    const ls200_rtsp_message *message,
                                    ls200_mutable_bytes *out_request,
                                    ls200_preview_access_unit *out_unit) {
  if (message->kind == LS200_RTSP_MESSAGE_RESPONSE) {
    return ls200_preview_consume_response(reader, message, out_request);
  }
  if (reader->state != LS200_PREVIEW_READER_STREAMING) {
    return LS200_STATUS_STATE_ERROR;
  }
  if (message->channel == 0U) {
    return ls200_preview_consume_rtp(reader, message->body, out_unit);
  }
  return message->channel == 1U ? LS200_STATUS_AGAIN :
                                  LS200_STATUS_INVALID_DATA;
}

static int push_inputs_are_valid(const ls200_preview_reader *reader,
                                 ls200_bytes input,
                                 const ls200_mutable_bytes *out_request,
                                 const ls200_preview_access_unit *out_unit) {
  if (reader == NULL || !ls200_preview_output_is_valid(out_request) ||
      out_unit == NULL || (input.data == NULL && input.length != 0U)) {
    return 0;
  }
  return reader->state != LS200_PREVIEW_READER_NEW &&
         reader->state != LS200_PREVIEW_READER_FAILED &&
         reader->state != LS200_PREVIEW_READER_CLOSED;
}

ls200_status ls200_preview_reader_push(ls200_preview_reader *reader,
                                       ls200_bytes input,
                                       ls200_mutable_bytes *out_request,
                                       ls200_preview_access_unit *out_unit) {
  ls200_rtsp_message message;
  ls200_status status;
  if (!push_inputs_are_valid(reader, input, out_request, out_unit)) {
    return LS200_STATUS_STATE_ERROR;
  }
  ls200_preview_clear_outputs(out_request, out_unit);
  status = ls200_rtsp_stream_parser_push(reader->parser, input, &message);
  while (status == LS200_STATUS_OK) {
    status = consume_message(reader, &message, out_request, out_unit);
    if (status == LS200_STATUS_OK &&
        (out_request->length != 0U || out_unit->annex_b.length != 0U)) {
      return LS200_STATUS_OK;
    }
    if (status != LS200_STATUS_OK && status != LS200_STATUS_AGAIN) {
      reader->state = LS200_PREVIEW_READER_FAILED;
      return status;
    }
    status = ls200_rtsp_stream_parser_push(
        reader->parser, (ls200_bytes){NULL, 0U}, &message);
  }
  if (status != LS200_STATUS_AGAIN) reader->state = LS200_PREVIEW_READER_FAILED;
  return status;
}

ls200_status ls200_preview_reader_drain(ls200_preview_reader *reader,
                                        ls200_mutable_bytes *out_request,
                                        ls200_preview_access_unit *out_unit) {
  return ls200_preview_reader_push(reader, (ls200_bytes){NULL, 0U},
                                   out_request, out_unit);
}

ls200_status ls200_preview_reader_eof(ls200_preview_reader *reader) {
  if (reader == NULL || reader->state == LS200_PREVIEW_READER_NEW ||
      reader->state == LS200_PREVIEW_READER_CLOSED) {
    return LS200_STATUS_STATE_ERROR;
  }
  reader->state = LS200_PREVIEW_READER_CLOSED;
  return LS200_STATUS_END;
}

ls200_preview_reader_state ls200_preview_reader_get_state(
    const ls200_preview_reader *reader) {
  return reader == NULL ? LS200_PREVIEW_READER_FAILED : reader->state;
}

void ls200_preview_reader_destroy(ls200_preview_reader *reader) {
  if (reader == NULL) return;
  ls200_h264_depacketizer_destroy(reader->depacketizer);
  ls200_rtsp_stream_parser_destroy(reader->parser);
  if (reader->assembly != NULL) {
    ls200_preview_secure_zero(reader->assembly,
                              reader->config.maximum_access_unit_bytes);
    free(reader->assembly);
  }
  ls200_preview_secure_zero(reader, sizeof(*reader));
  free(reader);
}
