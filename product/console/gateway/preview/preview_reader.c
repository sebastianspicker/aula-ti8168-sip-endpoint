#include "preview_reader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void aula_preview_secure_zero(void *value, size_t length) {
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

int aula_preview_output_is_valid(const aula_mutable_bytes *output) {
  return output != NULL && (output->data != NULL || output->capacity == 0U);
}

void aula_preview_clear_outputs(aula_mutable_bytes *request,
                                 aula_preview_access_unit *unit) {
  request->length = 0U;
  (void)memset(unit, 0, sizeof(*unit));
}

aula_status aula_preview_reader_create(
    const aula_preview_reader_config *config,
    aula_preview_reader **out_reader) {
  aula_preview_reader *reader;
  aula_status status;
  int allocation_failed;
  int written;
  if (config == NULL || out_reader == NULL || config->target_port == 0U ||
      !target_is_allowed(config->target_ipv4) ||
      config->maximum_access_unit_bytes < AULA_PREVIEW_MIN_ACCESS_UNIT_BYTES ||
      config->maximum_access_unit_bytes >
          AULA_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  *out_reader = NULL;
  reader = (aula_preview_reader *)calloc(1U, sizeof(*reader));
  if (reader == NULL) return AULA_STATUS_INTERNAL_ERROR;
  reader->config = *config;
  written = snprintf(reader->aggregate_uri, sizeof(reader->aggregate_uri),
                     "rtsp://%u.%u.%u.%u:%u%s", config->target_ipv4[0],
                     config->target_ipv4[1], config->target_ipv4[2],
                     config->target_ipv4[3], config->target_port,
                     AULA_PREVIEW_MOVIE_PATH);
  if (written < 0 || (size_t)written >= sizeof(reader->aggregate_uri)) {
    free(reader);
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  reader->assembly = (uint8_t *)malloc(config->maximum_access_unit_bytes);
  status = aula_rtsp_stream_parser_create(&reader->parser);
  if (status == AULA_STATUS_OK) {
    status = aula_h264_depacketizer_create(config->maximum_access_unit_bytes,
                                            &reader->depacketizer);
  }
  allocation_failed = reader->assembly == NULL;
  if (allocation_failed || status != AULA_STATUS_OK) {
    aula_h264_depacketizer_destroy(reader->depacketizer);
    aula_rtsp_stream_parser_destroy(reader->parser);
    free(reader->assembly);
    aula_preview_secure_zero(reader, sizeof(*reader));
    free(reader);
    return allocation_failed ? AULA_STATUS_INTERNAL_ERROR : status;
  }
  reader->state = AULA_PREVIEW_READER_NEW;
  *out_reader = reader;
  return AULA_STATUS_OK;
}

aula_status aula_preview_reader_start(aula_preview_reader *reader,
                                        aula_mutable_bytes *out_request) {
  if (reader == NULL || !aula_preview_output_is_valid(out_request) ||
      reader->state != AULA_PREVIEW_READER_NEW) {
    return AULA_STATUS_STATE_ERROR;
  }
  out_request->length = 0U;
  reader->next_cseq = 1U;
  return aula_preview_render_request(
      reader, "OPTIONS", reader->aggregate_uri, "",
      AULA_PREVIEW_READER_OPTIONS, out_request);
}

static aula_status consume_message(aula_preview_reader *reader,
                                    const aula_rtsp_message *message,
                                    aula_mutable_bytes *out_request,
                                    aula_preview_access_unit *out_unit) {
  if (message->kind == AULA_RTSP_MESSAGE_RESPONSE) {
    return aula_preview_consume_response(reader, message, out_request);
  }
  if (reader->state != AULA_PREVIEW_READER_STREAMING) {
    return AULA_STATUS_STATE_ERROR;
  }
  if (message->channel == 0U) {
    return aula_preview_consume_rtp(reader, message->body, out_unit);
  }
  return message->channel == 1U ? AULA_STATUS_AGAIN :
                                  AULA_STATUS_INVALID_DATA;
}

static int push_inputs_are_valid(const aula_preview_reader *reader,
                                 aula_bytes input,
                                 const aula_mutable_bytes *out_request,
                                 const aula_preview_access_unit *out_unit) {
  if (reader == NULL || !aula_preview_output_is_valid(out_request) ||
      out_unit == NULL || (input.data == NULL && input.length != 0U)) {
    return 0;
  }
  return reader->state != AULA_PREVIEW_READER_NEW &&
         reader->state != AULA_PREVIEW_READER_FAILED &&
         reader->state != AULA_PREVIEW_READER_CLOSED;
}

aula_status aula_preview_reader_push(aula_preview_reader *reader,
                                       aula_bytes input,
                                       aula_mutable_bytes *out_request,
                                       aula_preview_access_unit *out_unit) {
  aula_rtsp_message message;
  aula_status status;
  if (!push_inputs_are_valid(reader, input, out_request, out_unit)) {
    return AULA_STATUS_STATE_ERROR;
  }
  aula_preview_clear_outputs(out_request, out_unit);
  status = aula_rtsp_stream_parser_push(reader->parser, input, &message);
  while (status == AULA_STATUS_OK) {
    status = consume_message(reader, &message, out_request, out_unit);
    if (status == AULA_STATUS_OK &&
        (out_request->length != 0U || out_unit->annex_b.length != 0U)) {
      return AULA_STATUS_OK;
    }
    if (status != AULA_STATUS_OK && status != AULA_STATUS_AGAIN) {
      reader->state = AULA_PREVIEW_READER_FAILED;
      return status;
    }
    status = aula_rtsp_stream_parser_push(
        reader->parser, (aula_bytes){NULL, 0U}, &message);
  }
  if (status != AULA_STATUS_AGAIN) reader->state = AULA_PREVIEW_READER_FAILED;
  return status;
}

aula_status aula_preview_reader_drain(aula_preview_reader *reader,
                                        aula_mutable_bytes *out_request,
                                        aula_preview_access_unit *out_unit) {
  return aula_preview_reader_push(reader, (aula_bytes){NULL, 0U},
                                   out_request, out_unit);
}

aula_status aula_preview_reader_eof(aula_preview_reader *reader) {
  if (reader == NULL || reader->state == AULA_PREVIEW_READER_NEW ||
      reader->state == AULA_PREVIEW_READER_CLOSED) {
    return AULA_STATUS_STATE_ERROR;
  }
  reader->state = AULA_PREVIEW_READER_CLOSED;
  return AULA_STATUS_END;
}

aula_preview_reader_state aula_preview_reader_get_state(
    const aula_preview_reader *reader) {
  return reader == NULL ? AULA_PREVIEW_READER_FAILED : reader->state;
}

void aula_preview_reader_destroy(aula_preview_reader *reader) {
  if (reader == NULL) return;
  aula_h264_depacketizer_destroy(reader->depacketizer);
  aula_rtsp_stream_parser_destroy(reader->parser);
  if (reader->assembly != NULL) {
    aula_preview_secure_zero(reader->assembly,
                              reader->config.maximum_access_unit_bytes);
    free(reader->assembly);
  }
  aula_preview_secure_zero(reader, sizeof(*reader));
  free(reader);
}
