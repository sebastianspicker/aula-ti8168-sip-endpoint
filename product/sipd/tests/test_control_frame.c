/* LSZ1 frame encoding and decoding: the typed trust boundary between the
 * gateway and the daemon. */
#include "aula_sipd/control_protocol.h"
#include "aula_test.h"

#include <stdint.h>

static void test_round_trip(void) {
  static const char payload[] = "{\"digits\":\"12#\"}";
  uint8_t wire[AULA_CONTROL_HEADER_BYTES + sizeof(payload)];
  aula_mutable_bytes output = {wire, sizeof(wire), 0U};
  aula_control_frame frame;
  aula_control_frame decoded;
  (void)memset(&frame, 0, sizeof(frame));
  frame.opcode = AULA_CONTROL_OPCODE_DTMF;
  frame.flags = AULA_CONTROL_FRAME_RESPONSE;
  frame.request_id = 0x01020304U;
  frame.payload.data = (const uint8_t *)payload;
  frame.payload.length = sizeof(payload) - 1U;
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output), AULA_STATUS_OK);
  AULA_CHECK(output.length == AULA_CONTROL_HEADER_BYTES + sizeof(payload) - 1U);
  AULA_CHECK(memcmp(wire, "LSZ1", 4U) == 0);
  AULA_CHECK(wire[4] == AULA_CONTROL_PROTOCOL_VERSION);
  AULA_CHECK(wire[5] == (uint8_t)AULA_CONTROL_OPCODE_DTMF);
  /* Integers are big-endian on the wire. */
  AULA_CHECK(wire[6] == 0U && wire[7] == 1U);
  AULA_CHECK(wire[8] == 1U && wire[9] == 2U && wire[10] == 3U && wire[11] == 4U);
  AULA_CHECK(wire[15] == (uint8_t)(sizeof(payload) - 1U));
  AULA_CHECK_STATUS(aula_control_frame_decode(
      (aula_bytes){wire, output.length}, &decoded), AULA_STATUS_OK);
  AULA_CHECK(decoded.opcode == AULA_CONTROL_OPCODE_DTMF);
  AULA_CHECK(decoded.flags == AULA_CONTROL_FRAME_RESPONSE);
  AULA_CHECK(decoded.request_id == 0x01020304U);
  AULA_CHECK(decoded.payload.length == sizeof(payload) - 1U);
  AULA_CHECK(memcmp(decoded.payload.data, payload, sizeof(payload) - 1U) == 0);
}

static void test_encode_rejects_invalid_frames(void) {
  uint8_t wire[AULA_CONTROL_HEADER_BYTES];
  aula_mutable_bytes output = {wire, sizeof(wire), 0U};
  aula_control_frame frame;
  (void)memset(&frame, 0, sizeof(frame));
  frame.opcode = (aula_control_opcode)0;
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output),
                    AULA_STATUS_INVALID_ARGUMENT);
  frame.opcode = (aula_control_opcode)(AULA_CONTROL_OPCODE_METRICS + 1);
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output),
                    AULA_STATUS_INVALID_ARGUMENT);
  frame.opcode = AULA_CONTROL_OPCODE_STATUS;
  frame.flags = 1U << 3;
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output),
                    AULA_STATUS_INVALID_ARGUMENT);
  frame.flags = 0U;
  frame.payload.length = 1U;
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output),
                    AULA_STATUS_INVALID_ARGUMENT);
  frame.payload.data = (const uint8_t *)"{";
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output),
                    AULA_STATUS_LIMIT_EXCEEDED);
  frame.payload.length = 0U;
  AULA_CHECK_STATUS(aula_control_frame_encode(&frame, &output), AULA_STATUS_OK);
  AULA_CHECK(output.length == AULA_CONTROL_HEADER_BYTES);
}

static void encode_header(uint8_t wire[AULA_CONTROL_HEADER_BYTES],
                          uint8_t opcode, uint32_t payload_length) {
  (void)memcpy(wire, "LSZ1", 4U);
  wire[4] = AULA_CONTROL_PROTOCOL_VERSION;
  wire[5] = opcode;
  wire[6] = 0U;
  wire[7] = 0U;
  wire[8] = wire[9] = wire[10] = 0U;
  wire[11] = 7U;
  wire[12] = (uint8_t)(payload_length >> 24);
  wire[13] = (uint8_t)(payload_length >> 16);
  wire[14] = (uint8_t)(payload_length >> 8);
  wire[15] = (uint8_t)payload_length;
}

static void test_decode_rejects_malformed_input(void) {
  uint8_t wire[AULA_CONTROL_HEADER_BYTES + 2U];
  aula_control_frame frame;
  encode_header(wire, AULA_CONTROL_OPCODE_STATUS, 2U);
  wire[16] = '{';
  wire[17] = '}';
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_OK);
  AULA_CHECK(frame.request_id == 7U);
  /* Truncated header and payload-length mismatches. */
  AULA_CHECK_STATUS(aula_control_frame_decode(
      (aula_bytes){wire, AULA_CONTROL_HEADER_BYTES - 1U}, &frame),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_frame_decode(
      (aula_bytes){wire, sizeof(wire) - 1U}, &frame), AULA_STATUS_INVALID_DATA);
  /* Wrong magic, version, opcode and reserved flag bits. */
  wire[0] = 'X';
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_INVALID_DATA);
  encode_header(wire, AULA_CONTROL_OPCODE_STATUS, 2U);
  wire[4] = AULA_CONTROL_PROTOCOL_VERSION + 1U;
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_INVALID_DATA);
  encode_header(wire, 0U, 2U);
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_INVALID_DATA);
  encode_header(wire, AULA_CONTROL_OPCODE_STATUS, 2U);
  wire[7] = 1U << 3;
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_INVALID_DATA);
  /* A declared payload above the protocol maximum is refused outright. */
  encode_header(wire, AULA_CONTROL_OPCODE_STATUS, AULA_CONTROL_MAX_PAYLOAD_BYTES + 1U);
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){wire, sizeof(wire)}, &frame),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_frame_decode((aula_bytes){NULL, 0U}, &frame),
                    AULA_STATUS_INVALID_DATA);
}

int main(void) {
  test_round_trip();
  test_encode_rejects_invalid_frames();
  test_decode_rejects_malformed_input();
  return aula_test_result("test_control_frame");
}
