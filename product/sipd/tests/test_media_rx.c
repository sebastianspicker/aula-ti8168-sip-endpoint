/* Receive-path regressions: playout empty payloads and sequence wrap, RTSP
 * parser free space, and receiver-report loss clamping. */
#include "backends/rtsp_private.h"
#include "media/rx_playout.h"
#include "media/rx_shim_private.h"
#include "aula_sipd/h264.h"
#include "aula_sipd/rtcp.h"
#include "aula_test.h"

#include <stdint.h>

static aula_rtp_packet make_packet(uint16_t sequence, const uint8_t *data,
                                   size_t length) {
  aula_rtp_packet packet;
  (void)memset(&packet, 0, sizeof(packet));
  packet.header.sequence_number = sequence;
  packet.payload.data = data;
  packet.payload.length = length;
  return packet;
}

static void test_playout_empty_payload_and_wrap(void) {
  static const uint8_t byte = 0x5aU;
  static const uint16_t sequences[] = {65534U, 65535U, 0U, 1U};
  aula_rx_playout playout;
  aula_rtp_packet packet = make_packet(10U, &byte, 0U);
  aula_rtp_packet out;
  uint8_t payload[16];
  int discontinuity = 0;
  size_t index;
  aula_rx_playout_init(&playout, 1U, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 0U), AULA_STATUS_AGAIN);
  AULA_CHECK(playout.stats.dropped_packets == 1U);
  packet = make_packet(10U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 0U), AULA_STATUS_OK);
  aula_rx_playout_init(&playout, 1U, 1U);
  for (index = 0U; index < 4U; ++index) {
    packet = make_packet(sequences[index], &byte, 1U);
    AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 0U), AULA_STATUS_OK);
  }
  for (index = 0U; index < 4U; ++index) {
    AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 1000U * (index + 1U), &out,
                                           payload, sizeof(payload), &discontinuity),
                      AULA_STATUS_OK);
    AULA_CHECK(out.header.sequence_number == sequences[index]);
    AULA_CHECK(discontinuity == 0);
  }
}

static void test_playout_resyncs_after_sequence_jump(void) {
  static const uint8_t byte = 0x5aU;
  aula_rx_playout playout;
  aula_rtp_packet packet;
  aula_rtp_packet out;
  uint8_t payload[16];
  int discontinuity = 0;
  aula_rx_playout_init(&playout, 1U, 1U);
  packet = make_packet(100U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 0U), AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 1U, &out, payload, sizeof(payload),
                                         &discontinuity), AULA_STATUS_OK);
  /* One stray far-ahead packet must not stall the real stream. */
  packet = make_packet(20100U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 2U), AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 10U, &out, payload, sizeof(payload),
                                         &discontinuity), AULA_STATUS_OK);
  packet = make_packet(102U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 11U), AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 12U, &out, payload, sizeof(payload),
                                         &discontinuity), AULA_STATUS_OK);
  AULA_CHECK(out.header.sequence_number == 102U);
  AULA_CHECK(discontinuity == 1);
  /* A sender that restarts its sequence on the same SSRC is followed too. */
  packet = make_packet(5U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 13U), AULA_STATUS_OK);
}

static aula_bytes interleaved_partial(uint8_t *buffer, size_t length) {
  (void)memset(buffer, 0, length);
  buffer[0] = '$';
  buffer[2] = (uint8_t)(AULA_SIPD_MAX_RTP_PACKET_BYTES >> 8U);
  buffer[3] = (uint8_t)(AULA_SIPD_MAX_RTP_PACKET_BYTES & 0xffU);
  return (aula_bytes){buffer, length};
}

static void test_parser_room(void) {
  static uint8_t partial[100];
  static uint8_t filler[AULA_RTSP_STREAM_BUFFER_BYTES + 1U];
  aula_rtsp_stream_parser *parser = NULL;
  aula_rtsp_message message;
  size_t room;
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_create(&parser), AULA_STATUS_OK);
  if (parser == NULL) return;
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_push(
      parser, interleaved_partial(partial, sizeof(partial)), &message), AULA_STATUS_AGAIN);
  room = aula_rtsp_stream_parser_room(parser);
  AULA_CHECK(room == AULA_RTSP_STREAM_BUFFER_BYTES - sizeof(partial));
  AULA_CHECK(aula_rtsp_stream_parser_push(
      parser, (aula_bytes){filler, room}, &message) != AULA_STATUS_LIMIT_EXCEEDED);
  aula_rtsp_stream_parser_destroy(parser);
  parser = NULL;
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_create(&parser), AULA_STATUS_OK);
  if (parser == NULL) return;
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_push(
      parser, interleaved_partial(partial, sizeof(partial)), &message), AULA_STATUS_AGAIN);
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_push(
      parser, (aula_bytes){filler, room + 1U}, &message), AULA_STATUS_LIMIT_EXCEEDED);
  aula_rtsp_stream_parser_destroy(parser);
  parser = NULL;
  /* A returned message stays buffered until the next push discards it. */
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_create(&parser), AULA_STATUS_OK);
  if (parser == NULL) return;
  partial[0] = '$';
  partial[1] = 0U;
  partial[2] = 0U;
  partial[3] = 4U;
  AULA_CHECK_STATUS(aula_rtsp_stream_parser_push(
      parser, (aula_bytes){partial, 8U + 50U}, &message), AULA_STATUS_OK);
  AULA_CHECK(aula_rtsp_stream_parser_room(parser) == AULA_RTSP_STREAM_BUFFER_BYTES - 50U);
  aula_rtsp_stream_parser_destroy(parser);
}

static void test_receiver_report_loss_clamp(void) {
  aula_rx_source_entry entry;
  aula_rtcp_report_metrics metrics;
  uint8_t data[64];
  aula_mutable_bytes output = {data, sizeof(data), 0U};
  uint16_t sequence = 0U;
  (void)memset(&entry, 0, sizeof(entry));
  entry.stats.ssrc = 2U;
  (void)aula_rx_shim_account_sequence(&entry, sequence);
  while ((uint64_t)entry.highest_sequence_extended + 1U - entry.unique_received <= 8388607U) {
    sequence = (uint16_t)(sequence + 32767U);
    (void)aula_rx_shim_account_sequence(&entry, sequence);
  }
  aula_rx_shim_report_metrics(&entry, 1U, 0U, &metrics);
  AULA_CHECK(metrics.cumulative_loss == 8388607);
  AULA_CHECK_STATUS(aula_rtcp_build_receiver_report(&metrics, &output), AULA_STATUS_OK);
}

static void test_rtsp_uri_rejects_trailing_parent(void) {
  aula_rtsp_uri uri;
  AULA_CHECK_STATUS(aula_rtsp_parse_authorized_uri("rtsp://192.168.1.10/stream",
                                                   "192.168.1.10", &uri), AULA_STATUS_OK);
  AULA_CHECK(aula_rtsp_parse_authorized_uri("rtsp://192.168.1.10/stream/..",
                                            "192.168.1.10", &uri) != AULA_STATUS_OK);
}

static void test_playout_skip_keeps_sequence_continuous(void) {
  static const uint8_t byte = 0x5aU;
  aula_rx_playout playout;
  aula_rtp_packet packet = make_packet(10U, &byte, 1U);
  aula_rtp_packet out;
  uint8_t payload[16];
  int discontinuity = 1;
  aula_rx_playout_init(&playout, 1U, 1000U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 0U), AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 0U, &out, payload, sizeof(payload),
                                         &discontinuity), AULA_STATUS_OK);
  aula_rx_playout_skip(&playout, 11U);
  packet = make_packet(12U, &byte, 1U);
  AULA_CHECK_STATUS(aula_rx_playout_push(&playout, &packet, 1U), AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_rx_playout_take(&playout, 2U, &out, payload, sizeof(payload),
                                         &discontinuity), AULA_STATUS_OK);
  AULA_CHECK(out.header.sequence_number == 12U);
  AULA_CHECK(discontinuity == 0);
}

int main(void) {
  test_playout_empty_payload_and_wrap();
  test_playout_resyncs_after_sequence_jump();
  test_playout_skip_keeps_sequence_continuous();
  test_parser_room();
  test_rtsp_uri_rejects_trailing_parent();
  test_receiver_report_loss_clamp();
  return aula_test_result("test_media_rx");
}
