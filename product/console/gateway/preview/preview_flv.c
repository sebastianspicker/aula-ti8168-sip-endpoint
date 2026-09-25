#include "preview_flv.h"

#include "ls200_sipd/h264.h"

#include <limits.h>
#include <string.h>

#define FLV_TAG_HEADER_BYTES 11U
#define FLV_PREVIOUS_TAG_SIZE_BYTES 4U
#define FLV_AVC_PACKET_HEADER_BYTES 5U

typedef struct rbsp_reader {
  const uint8_t *data;
  size_t length;
  size_t raw_offset;
  uint8_t byte;
  uint8_t bits_left;
  uint8_t zero_count;
} rbsp_reader;

static void write_u16(uint8_t output[2], uint16_t value) {
  output[0] = (uint8_t)(value >> 8U);
  output[1] = (uint8_t)value;
}

static void write_u24(uint8_t output[3], uint32_t value) {
  output[0] = (uint8_t)(value >> 16U);
  output[1] = (uint8_t)(value >> 8U);
  output[2] = (uint8_t)value;
}

static void write_u32(uint8_t output[4], uint32_t value) {
  output[0] = (uint8_t)(value >> 24U);
  output[1] = (uint8_t)(value >> 16U);
  output[2] = (uint8_t)(value >> 8U);
  output[3] = (uint8_t)value;
}

static ls200_status emit(ls200_preview_flv_mux *mux,
                         ls200_preview_write_fn write_fn, void *context,
                         const uint8_t *data, size_t length) {
  ls200_status status;
  if (length == 0U) return LS200_STATUS_OK;
  status = write_fn(context, (ls200_bytes){data, length});
  if (status != LS200_STATUS_OK) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_WRITE_FAILED;
  }
  return status;
}

static ls200_status emit_tag_header(ls200_preview_flv_mux *mux,
                                    ls200_preview_write_fn write_fn,
                                    void *context, uint32_t data_size,
                                    uint32_t timestamp_ms) {
  uint8_t header[FLV_TAG_HEADER_BYTES] = {0U};
  if (data_size > 0x00ffffffU) return LS200_STATUS_LIMIT_EXCEEDED;
  header[0] = 9U;
  write_u24(header + 1U, data_size);
  write_u24(header + 4U, timestamp_ms & 0x00ffffffU);
  header[7] = (uint8_t)(timestamp_ms >> 24U);
  return emit(mux, write_fn, context, header, sizeof(header));
}

static ls200_status emit_previous_tag_size(ls200_preview_flv_mux *mux,
                                           ls200_preview_write_fn write_fn,
                                           void *context,
                                           uint32_t data_size) {
  uint8_t encoded[4];
  write_u32(encoded, data_size + FLV_TAG_HEADER_BYTES);
  return emit(mux, write_fn, context, encoded, sizeof(encoded));
}

static int rbsp_next_byte(rbsp_reader *reader, uint8_t *out_byte) {
  while (reader->raw_offset < reader->length) {
    uint8_t value = reader->data[reader->raw_offset++];
    if (reader->zero_count == 2U && value == 0x03U) {
      reader->zero_count = 0U;
      continue;
    }
    if (value == 0U) {
      if (reader->zero_count < 2U) reader->zero_count++;
    } else {
      reader->zero_count = 0U;
    }
    *out_byte = value;
    return 1;
  }
  return 0;
}

static int rbsp_read_bit(rbsp_reader *reader, uint32_t *out_bit) {
  if (reader->bits_left == 0U) {
    if (!rbsp_next_byte(reader, &reader->byte)) return 0;
    reader->bits_left = 8U;
  }
  reader->bits_left--;
  *out_bit = (reader->byte >> reader->bits_left) & 1U;
  return 1;
}

static int rbsp_read_ue(rbsp_reader *reader, uint32_t *out_value) {
  uint32_t bit;
  uint32_t suffix = 0U;
  unsigned int leading_zeroes = 0U;
  unsigned int index;
  while (rbsp_read_bit(reader, &bit) && bit == 0U) {
    if (++leading_zeroes > 30U) return 0;
  }
  if (bit == 0U) return 0;
  for (index = 0U; index < leading_zeroes; ++index) {
    if (!rbsp_read_bit(reader, &bit)) return 0;
    suffix = (suffix << 1U) | bit;
  }
  *out_value = ((UINT32_C(1) << leading_zeroes) - 1U) + suffix;
  return 1;
}

static ls200_status slice_timing_supported(const ls200_h264_nal *nal) {
  rbsp_reader reader;
  uint32_t first_macroblock;
  uint32_t slice_type;
  if (nal->type == 5U) return LS200_STATUS_OK;
  if (nal->type != 1U || nal->length < 2U) return LS200_STATUS_UNSUPPORTED;
  (void)memset(&reader, 0, sizeof(reader));
  reader.data = nal->data + 1U;
  reader.length = nal->length - 1U;
  if (!rbsp_read_ue(&reader, &first_macroblock) ||
      !rbsp_read_ue(&reader, &slice_type) || slice_type > 9U) {
    return LS200_STATUS_UNSUPPORTED;
  }
  (void)first_macroblock;
  return (slice_type % 5U) == 1U ? LS200_STATUS_UNSUPPORTED :
                                  LS200_STATUS_OK;
}

typedef struct access_unit_plan {
  uint32_t payload_bytes;
  int has_idr;
  int has_vcl;
} access_unit_plan;

static ls200_status plan_nal(const ls200_h264_nal *nal,
                             access_unit_plan *plan,
                             uint64_t *payload_bytes,
                             ls200_preview_flv_mux *mux) {
  ls200_status status;
  if (nal->type == 7U || nal->type == 8U) return LS200_STATUS_OK;
  if (nal->type >= 1U && nal->type <= 5U) {
    plan->has_vcl = 1;
    if (nal->type == 5U) plan->has_idr = 1;
    status = slice_timing_supported(nal);
    if (status != LS200_STATUS_OK) {
      mux->last_error = LS200_PREVIEW_FLV_ERROR_UNSUPPORTED_TIMING;
      return LS200_STATUS_UNSUPPORTED;
    }
  } else if (nal->type == 19U || nal->type == 20U || nal->type == 21U) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_UNSUPPORTED_TIMING;
    return LS200_STATUS_UNSUPPORTED;
  }
  *payload_bytes += 4U + nal->length;
  return *payload_bytes > 0x00ffffffU ? LS200_STATUS_LIMIT_EXCEEDED :
                                       LS200_STATUS_OK;
}

static ls200_status plan_access_unit(const ls200_preview_access_unit *unit,
                                     access_unit_plan *out_plan,
                                     ls200_preview_flv_mux *mux) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  ls200_status status;
  uint64_t payload_bytes = FLV_AVC_PACKET_HEADER_BYTES;
  (void)memset(out_plan, 0, sizeof(*out_plan));
  status = ls200_h264_annexb_iterator_init(
      &iterator, unit->annex_b, LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES);
  if (status != LS200_STATUS_OK) return status;
  for (;;) {
    status = ls200_h264_annexb_iterator_next(&iterator, &nal);
    if (status == LS200_STATUS_END) break;
    if (status != LS200_STATUS_OK) return status;
    status = plan_nal(&nal, out_plan, &payload_bytes, mux);
    if (status != LS200_STATUS_OK) return status;
  }
  if (out_plan->has_vcl == 0 || out_plan->has_idr != (unit->keyframe != 0)) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT;
    return LS200_STATUS_INVALID_DATA;
  }
  out_plan->payload_bytes = (uint32_t)payload_bytes;
  return LS200_STATUS_OK;
}

static ls200_status emit_flv_header(ls200_preview_flv_mux *mux,
                                    ls200_preview_write_fn write_fn,
                                    void *context) {
  static const uint8_t header[] = {
    'F', 'L', 'V', 0x01U, 0x01U, 0x00U, 0x00U, 0x00U, 0x09U,
    0x00U, 0x00U, 0x00U, 0x00U
  };
  ls200_status status = emit(mux, write_fn, context, header, sizeof(header));
  if (status == LS200_STATUS_OK) mux->header_written = 1;
  return status;
}

static ls200_status emit_decoder_config(
    ls200_preview_flv_mux *mux, const ls200_preview_access_unit *unit,
    uint32_t timestamp_ms, ls200_preview_write_fn write_fn, void *context) {
  uint8_t fixed[16];
  uint8_t length[2];
  uint32_t data_size;
  ls200_status status;
  int sps_valid = unit->sps.data != NULL && unit->sps.length >= 4U &&
                  unit->sps.length <= 1024U;
  int pps_valid = unit->pps.data != NULL && unit->pps.length != 0U &&
                  unit->pps.length <= 1024U;
  if (!sps_valid || !pps_valid) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT;
    return LS200_STATUS_INVALID_DATA;
  }
  data_size = (uint32_t)(16U + unit->sps.length + unit->pps.length);
  status = emit_tag_header(mux, write_fn, context, data_size, timestamp_ms);
  if (status != LS200_STATUS_OK) return status;
  fixed[0] = 0x17U;
  fixed[1] = 0x00U;
  fixed[2] = 0x00U;
  fixed[3] = 0x00U;
  fixed[4] = 0x00U;
  fixed[5] = 0x01U;
  fixed[6] = unit->sps.data[1];
  fixed[7] = unit->sps.data[2];
  fixed[8] = unit->sps.data[3];
  fixed[9] = 0xffU;
  fixed[10] = 0xe1U;
  write_u16(fixed + 11U, (uint16_t)unit->sps.length);
  status = emit(mux, write_fn, context, fixed, 13U);
  if (status != LS200_STATUS_OK) return status;
  status = emit(mux, write_fn, context, unit->sps.data, unit->sps.length);
  if (status != LS200_STATUS_OK) return status;
  fixed[0] = 0x01U;
  write_u16(length, (uint16_t)unit->pps.length);
  status = emit(mux, write_fn, context, fixed, 1U);
  if (status != LS200_STATUS_OK) return status;
  status = emit(mux, write_fn, context, length, sizeof(length));
  if (status != LS200_STATUS_OK) return status;
  status = emit(mux, write_fn, context, unit->pps.data, unit->pps.length);
  if (status != LS200_STATUS_OK) return status;
  status = emit_previous_tag_size(mux, write_fn, context, data_size);
  if (status == LS200_STATUS_OK) {
    mux->decoder_config_written = 1;
    mux->parameter_set_generation = unit->parameter_set_generation;
  }
  return status;
}

static ls200_status emit_video_tag(ls200_preview_flv_mux *mux,
                                   const ls200_preview_access_unit *unit,
                                   const access_unit_plan *plan,
                                   uint32_t timestamp_ms,
                                   ls200_preview_write_fn write_fn,
                                   void *context) {
  ls200_h264_annexb_iterator iterator;
  ls200_h264_nal nal;
  uint8_t avc_header[FLV_AVC_PACKET_HEADER_BYTES] = {0U};
  uint8_t length[4];
  ls200_status status;
  status = emit_tag_header(mux, write_fn, context, plan->payload_bytes,
                           timestamp_ms);
  if (status != LS200_STATUS_OK) return status;
  avc_header[0] = unit->keyframe != 0 ? 0x17U : 0x27U;
  avc_header[1] = 0x01U;
  status = emit(mux, write_fn, context, avc_header, sizeof(avc_header));
  if (status != LS200_STATUS_OK) return status;
  status = ls200_h264_annexb_iterator_init(
      &iterator, unit->annex_b, LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES);
  if (status != LS200_STATUS_OK) return status;
  for (;;) {
    status = ls200_h264_annexb_iterator_next(&iterator, &nal);
    if (status == LS200_STATUS_END) break;
    if (status != LS200_STATUS_OK) return status;
    if (nal.type == 7U || nal.type == 8U) continue;
    if (nal.length > UINT32_MAX) return LS200_STATUS_LIMIT_EXCEEDED;
    write_u32(length, (uint32_t)nal.length);
    status = emit(mux, write_fn, context, length, sizeof(length));
    if (status != LS200_STATUS_OK) return status;
    status = emit(mux, write_fn, context, nal.data, nal.length);
    if (status != LS200_STATUS_OK) return status;
  }
  return emit_previous_tag_size(mux, write_fn, context, plan->payload_bytes);
}

void ls200_preview_flv_mux_init(ls200_preview_flv_mux *mux) {
  if (mux != NULL) (void)memset(mux, 0, sizeof(*mux));
}

static int mux_input_is_valid(const ls200_preview_flv_mux *mux,
                              const ls200_preview_access_unit *unit,
                              ls200_preview_write_fn write_fn) {
  if (mux == NULL || unit == NULL || write_fn == NULL) return 0;
  if (unit->annex_b.data == NULL || unit->annex_b.length == 0U) return 0;
  return unit->annex_b.length <= LS200_SIPD_MAX_VIDEO_ACCESS_UNIT_BYTES &&
         unit->parameter_set_generation != 0U;
}

static ls200_status mux_timestamp(ls200_preview_flv_mux *mux,
                                  const ls200_preview_access_unit *unit,
                                  uint32_t *out_timestamp_ms) {
  uint64_t delta;
  uint64_t milliseconds;
  if (mux->header_written == 0) {
    mux->first_timestamp_90khz = unit->timestamp_90khz;
    mux->last_timestamp_90khz = unit->timestamp_90khz;
  } else if (unit->timestamp_90khz < mux->last_timestamp_90khz) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT;
    return LS200_STATUS_INVALID_DATA;
  }
  delta = unit->timestamp_90khz - mux->first_timestamp_90khz;
  if (delta > UINT64_MAX / UINT64_C(1000)) {
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  milliseconds = (delta * UINT64_C(1000)) / UINT64_C(90000);
  if (milliseconds > UINT32_MAX) return LS200_STATUS_LIMIT_EXCEEDED;
  *out_timestamp_ms = (uint32_t)milliseconds;
  return LS200_STATUS_OK;
}

ls200_status ls200_preview_flv_mux_write(
    ls200_preview_flv_mux *mux, const ls200_preview_access_unit *unit,
    ls200_preview_write_fn write_fn, void *write_context) {
  access_unit_plan plan;
  uint32_t timestamp_ms;
  int config_required;
  ls200_status status;
  if (!mux_input_is_valid(mux, unit, write_fn)) {
    if (mux != NULL) mux->last_error = LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT;
    return LS200_STATUS_INVALID_ARGUMENT;
  }
  mux->last_error = LS200_PREVIEW_FLV_ERROR_NONE;
  status = plan_access_unit(unit, &plan, mux);
  if (status != LS200_STATUS_OK) return status;
  config_required = mux->decoder_config_written == 0 || unit->discontinuity != 0 ||
                    mux->parameter_set_generation !=
                        unit->parameter_set_generation;
  if ((mux->header_written == 0 || config_required) && unit->keyframe == 0) {
    mux->last_error = LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT;
    return LS200_STATUS_STATE_ERROR;
  }
  status = mux_timestamp(mux, unit, &timestamp_ms);
  if (status != LS200_STATUS_OK) return status;
  if (mux->header_written == 0) {
    status = emit_flv_header(mux, write_fn, write_context);
    if (status != LS200_STATUS_OK) return status;
  }
  if (config_required) {
    status = emit_decoder_config(mux, unit, timestamp_ms, write_fn,
                                 write_context);
    if (status != LS200_STATUS_OK) return status;
  }
  status = emit_video_tag(mux, unit, &plan, timestamp_ms, write_fn,
                          write_context);
  if (status == LS200_STATUS_OK) {
    mux->last_timestamp_90khz = unit->timestamp_90khz;
  }
  return status;
}

ls200_preview_flv_error ls200_preview_flv_mux_last_error(
    const ls200_preview_flv_mux *mux) {
  return mux == NULL ? LS200_PREVIEW_FLV_ERROR_INVALID_ACCESS_UNIT :
                       mux->last_error;
}
