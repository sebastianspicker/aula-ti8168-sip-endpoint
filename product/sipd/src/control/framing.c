#include "ls200_sipd/control_protocol.h"

#include <arpa/inet.h>
#include <string.h>

static int opcode_is_known(uint8_t opcode) {
  return opcode >= (uint8_t)LS200_CONTROL_OPCODE_STATUS &&
      opcode <= (uint8_t)LS200_CONTROL_OPCODE_METRICS;
}

ls200_status ls200_control_frame_encode(const ls200_control_frame *frame,
                                        ls200_mutable_bytes *output) {
  uint32_t request_id;
  uint32_t payload_length;
  uint16_t flags;
  size_t required;
  if (frame == NULL || output == NULL || output->data == NULL ||
      !opcode_is_known((uint8_t)frame->opcode) ||
      frame->payload.length > LS200_CONTROL_MAX_PAYLOAD_BYTES ||
      (frame->payload.length != 0U && frame->payload.data == NULL) ||
      (frame->flags & ~(LS200_CONTROL_FRAME_RESPONSE |
                        LS200_CONTROL_FRAME_ERROR |
                        LS200_CONTROL_FRAME_EVENT)) != 0U)
    return LS200_STATUS_INVALID_ARGUMENT;
  required = LS200_CONTROL_HEADER_BYTES + frame->payload.length;
  if (required > output->capacity) return LS200_STATUS_LIMIT_EXCEEDED;
  (void)memcpy(output->data, LS200_CONTROL_MAGIC, 4U);
  output->data[4] = LS200_CONTROL_PROTOCOL_VERSION;
  output->data[5] = (uint8_t)frame->opcode;
  flags = (uint16_t)htons(frame->flags);
  request_id = htonl(frame->request_id);
  payload_length = htonl((uint32_t)frame->payload.length);
  (void)memcpy(output->data + 6U, &flags, sizeof(flags));
  (void)memcpy(output->data + 8U, &request_id, sizeof(request_id));
  (void)memcpy(output->data + 12U, &payload_length, sizeof(payload_length));
  if (frame->payload.length != 0U)
    (void)memcpy(output->data + LS200_CONTROL_HEADER_BYTES,
                 frame->payload.data, frame->payload.length);
  output->length = required;
  return LS200_STATUS_OK;
}

ls200_status ls200_control_frame_decode(ls200_bytes input,
                                        ls200_control_frame *out_frame) {
  uint32_t request_id;
  uint32_t payload_length;
  uint16_t flags;
  if (out_frame == NULL || input.data == NULL ||
      input.length < LS200_CONTROL_HEADER_BYTES)
    return LS200_STATUS_INVALID_DATA;
  if (memcmp(input.data, LS200_CONTROL_MAGIC, 4U) != 0 ||
      input.data[4] != LS200_CONTROL_PROTOCOL_VERSION ||
      !opcode_is_known(input.data[5]))
    return LS200_STATUS_INVALID_DATA;
  (void)memcpy(&flags, input.data + 6U, sizeof(flags));
  (void)memcpy(&request_id, input.data + 8U, sizeof(request_id));
  (void)memcpy(&payload_length, input.data + 12U, sizeof(payload_length));
  flags = (uint16_t)ntohs(flags);
  request_id = ntohl(request_id);
  payload_length = ntohl(payload_length);
  if ((flags & ~(LS200_CONTROL_FRAME_RESPONSE | LS200_CONTROL_FRAME_ERROR |
                 LS200_CONTROL_FRAME_EVENT)) != 0U ||
      payload_length > LS200_CONTROL_MAX_PAYLOAD_BYTES ||
      input.length != LS200_CONTROL_HEADER_BYTES + (size_t)payload_length)
    return LS200_STATUS_INVALID_DATA;
  (void)memset(out_frame, 0, sizeof(*out_frame));
  out_frame->opcode = (ls200_control_opcode)input.data[5];
  out_frame->flags = flags;
  out_frame->request_id = request_id;
  out_frame->payload.data = input.data + LS200_CONTROL_HEADER_BYTES;
  out_frame->payload.length = payload_length;
  return LS200_STATUS_OK;
}
