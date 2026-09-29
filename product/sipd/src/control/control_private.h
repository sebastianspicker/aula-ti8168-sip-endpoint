#ifndef AULA_SIPD_CONTROL_PRIVATE_H
#define AULA_SIPD_CONTROL_PRIVATE_H

#include "aula_sipd/control.h"
#include <sys/un.h>

#define AULA_CONTROL_COMPLETED_CACHE_ENTRIES 16U
#define AULA_CONTROL_CLIENT_LIMIT 64U
#define AULA_CONTROL_CONNECTION_TIMEOUT_NS UINT64_C(1000000000)
#define AULA_CONTROL_READINESS_DESCRIPTOR_LIMIT 8U
#define AULA_CONTROL_WIRE_BYTES \
  (AULA_CONTROL_HEADER_BYTES + AULA_CONTROL_MAX_PAYLOAD_BYTES)

typedef struct aula_control_client {
  int descriptor;
  uint64_t deadline_ns;
  uint8_t request[AULA_CONTROL_WIRE_BYTES];
  size_t request_length;
  uint8_t response[AULA_CONTROL_WIRE_BYTES];
  size_t response_length;
  size_t response_offset;
} aula_control_client;

typedef struct aula_control_completed_request {
  int used;
  aula_control_opcode opcode;
  uint32_t request_id;
  size_t request_length;
  size_t response_length;
  uint16_t response_flags;
  uint64_t request_fingerprint[2];
  uint8_t response_payload[AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES];
} aula_control_completed_request;

struct aula_control_server {
  int descriptor;
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  aula_control_config config;
  aula_endpoint_status status;
  uint8_t completed_cache_key[16];
  aula_control_completed_request completed[AULA_CONTROL_COMPLETED_CACHE_ENTRIES];
  size_t next_completed;
  aula_control_client clients[AULA_CONTROL_CLIENT_LIMIT];
  size_t client_count;
  size_t next_client;
};

aula_status aula_control_handle_client(aula_control_server *server,
                                         aula_control_client *client);
int aula_control_set_nonblocking(int descriptor);
int aula_control_peer_is_authorized(const aula_control_server *server,
                                      int descriptor);
void aula_control_request_fingerprint(
    const uint8_t key[16], aula_bytes payload, uint64_t output[2]);
aula_status aula_control_server_poll_with_readiness(
    aula_control_server *server, aula_deadline deadline,
    const int *readiness_descriptors, size_t readiness_count);
#endif
