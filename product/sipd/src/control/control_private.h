#ifndef LS200_SIPD_CONTROL_PRIVATE_H
#define LS200_SIPD_CONTROL_PRIVATE_H

#include "ls200_sipd/control.h"
#include <sys/un.h>

#define LS200_CONTROL_COMPLETED_CACHE_ENTRIES 16U
#define LS200_CONTROL_CLIENT_LIMIT 64U
#define LS200_CONTROL_CONNECTION_TIMEOUT_NS UINT64_C(1000000000)
#define LS200_CONTROL_READINESS_DESCRIPTOR_LIMIT 8U
#define LS200_CONTROL_WIRE_BYTES \
  (LS200_CONTROL_HEADER_BYTES + LS200_CONTROL_MAX_PAYLOAD_BYTES)

typedef struct ls200_control_client {
  int descriptor;
  uint64_t deadline_ns;
  uint8_t request[LS200_CONTROL_WIRE_BYTES];
  size_t request_length;
  uint8_t response[LS200_CONTROL_WIRE_BYTES];
  size_t response_length;
  size_t response_offset;
} ls200_control_client;

typedef struct ls200_control_completed_request {
  int used;
  ls200_control_opcode opcode;
  uint32_t request_id;
  size_t request_length;
  size_t response_length;
  uint16_t response_flags;
  uint64_t request_fingerprint[2];
  uint8_t response_payload[LS200_SIPD_MAX_CONTROL_MESSAGE_BYTES];
} ls200_control_completed_request;

struct ls200_control_server {
  int descriptor;
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  ls200_control_config config;
  ls200_endpoint_status status;
  uint8_t completed_cache_key[16];
  ls200_control_completed_request completed[LS200_CONTROL_COMPLETED_CACHE_ENTRIES];
  size_t next_completed;
  ls200_control_client clients[LS200_CONTROL_CLIENT_LIMIT];
  size_t client_count;
  size_t next_client;
};

ls200_status ls200_control_handle_client(ls200_control_server *server,
                                         ls200_control_client *client);
int ls200_control_set_nonblocking(int descriptor);
int ls200_control_peer_is_authorized(const ls200_control_server *server,
                                      int descriptor);
void ls200_control_request_fingerprint(
    const uint8_t key[16], ls200_bytes payload, uint64_t output[2]);
ls200_status ls200_control_server_poll_with_readiness(
    ls200_control_server *server, ls200_deadline deadline,
    const int *readiness_descriptors, size_t readiness_count);
#endif
