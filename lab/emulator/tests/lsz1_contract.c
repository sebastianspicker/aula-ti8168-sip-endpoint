/* Run the real gateway transport + response normalizer over an inherited
 * socketpair. Only connect is redirected: framing, deadlines, peer checks,
 * recvmsg/stream EOF and exact opcode schemas are production gateway code. */
#define _POSIX_C_SOURCE 200809L
#include "gateway.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int inherited = -1;
int contract_connect(int descriptor, const struct sockaddr *address, socklen_t length) {
  (void)address;
  (void)length;
  if (inherited < 0 || dup2(inherited, descriptor) < 0) return -1;
  return 0;
}

int main(int argc, char **argv) {
  aula_gateway_config config = {0};
  char response[4096] = {0};
  int result;
  if (argc != 7) return 64;
  inherited = atoi(argv[1]);
  config.control_socket_path = argv[2];
  config.expected_sipd_uid = (uint32_t)getuid();
  config.control_timeout_milliseconds = 100;
  result = aula_gateway_control_exchange(&config, (uint8_t)atoi(argv[3]),
      (uint32_t)strtoul(argv[4], NULL, 10), argv[5], response, sizeof(response));
  puts(response);
  return result == atoi(argv[6]) ? 0 : 1;
}
