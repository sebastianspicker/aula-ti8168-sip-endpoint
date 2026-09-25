#ifndef LS200_SIPD_CONTROL_PROTOCOL_H
#define LS200_SIPD_CONTROL_PROTOCOL_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS200_CONTROL_MAGIC "LSZ1"
#define LS200_CONTROL_PROTOCOL_VERSION 1U
#define LS200_CONTROL_HEADER_BYTES 16U
#define LS200_CONTROL_MAX_PAYLOAD_BYTES 65536U

typedef enum ls200_control_opcode {
  LS200_CONTROL_OPCODE_STATUS = 1,
  LS200_CONTROL_OPCODE_ORIGINATE = 2,
  LS200_CONTROL_OPCODE_HANGUP = 3,
  LS200_CONTROL_OPCODE_DTMF = 4,
  LS200_CONTROL_OPCODE_MEDIA = 5,
  LS200_CONTROL_OPCODE_REPLACE_CREDENTIAL = 6,
  LS200_CONTROL_OPCODE_DIAGNOSTICS = 7,
  LS200_CONTROL_OPCODE_SUBSCRIBE = 8,
  /* Additive: the established opcodes above are never renumbered. */
  LS200_CONTROL_OPCODE_SETTINGS = 9,
  /* Read-only bounded process/session metrics; STATUS remains unchanged. */
  LS200_CONTROL_OPCODE_METRICS = 10
} ls200_control_opcode;

typedef enum ls200_control_frame_flag {
  LS200_CONTROL_FRAME_RESPONSE = 1U << 0,
  LS200_CONTROL_FRAME_ERROR = 1U << 1,
  LS200_CONTROL_FRAME_EVENT = 1U << 2
} ls200_control_frame_flag;

typedef struct ls200_control_frame {
  ls200_control_opcode opcode;
  uint16_t flags;
  uint32_t request_id;
  ls200_bytes payload;
} ls200_control_frame;

/* Integer fields are encoded in network byte order. The payload is an
 * opcode-specific, bounded UTF-8 JSON object; the frame itself is the typed
 * trust boundary and never carries a raw SIP URI or executable input. */
ls200_status ls200_control_frame_encode(const ls200_control_frame *frame,
                                        ls200_mutable_bytes *output);
ls200_status ls200_control_frame_decode(ls200_bytes input,
                                        ls200_control_frame *out_frame);

#ifdef __cplusplus
}
#endif

#endif
