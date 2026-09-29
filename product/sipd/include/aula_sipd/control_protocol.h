#ifndef AULA_SIPD_CONTROL_PROTOCOL_H
#define AULA_SIPD_CONTROL_PROTOCOL_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AULA_CONTROL_MAGIC "LSZ1"
#define AULA_CONTROL_PROTOCOL_VERSION 1U
#define AULA_CONTROL_HEADER_BYTES 16U
#define AULA_CONTROL_MAX_PAYLOAD_BYTES 65536U

typedef enum aula_control_opcode {
  AULA_CONTROL_OPCODE_STATUS = 1,
  AULA_CONTROL_OPCODE_ORIGINATE = 2,
  AULA_CONTROL_OPCODE_HANGUP = 3,
  AULA_CONTROL_OPCODE_DTMF = 4,
  AULA_CONTROL_OPCODE_MEDIA = 5,
  AULA_CONTROL_OPCODE_REPLACE_CREDENTIAL = 6,
  AULA_CONTROL_OPCODE_DIAGNOSTICS = 7,
  AULA_CONTROL_OPCODE_SUBSCRIBE = 8,
  /* Additive: the established opcodes above are never renumbered. */
  AULA_CONTROL_OPCODE_SETTINGS = 9,
  /* Read-only bounded process/session metrics; STATUS remains unchanged. */
  AULA_CONTROL_OPCODE_METRICS = 10
} aula_control_opcode;

typedef enum aula_control_frame_flag {
  AULA_CONTROL_FRAME_RESPONSE = 1U << 0,
  AULA_CONTROL_FRAME_ERROR = 1U << 1,
  AULA_CONTROL_FRAME_EVENT = 1U << 2
} aula_control_frame_flag;

typedef struct aula_control_frame {
  aula_control_opcode opcode;
  uint16_t flags;
  uint32_t request_id;
  aula_bytes payload;
} aula_control_frame;

/* Integer fields are encoded in network byte order. The payload is an
 * opcode-specific, bounded UTF-8 JSON object; the frame itself is the typed
 * trust boundary and never carries a raw SIP URI or executable input. */
aula_status aula_control_frame_encode(const aula_control_frame *frame,
                                        aula_mutable_bytes *output);
aula_status aula_control_frame_decode(aula_bytes input,
                                        aula_control_frame *out_frame);

#ifdef __cplusplus
}
#endif

#endif
