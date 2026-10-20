/* Real device client and gateway response projection over an inherited fd. */
#define _POSIX_C_SOURCE 200809L
#include "gateway_internal.h"
#include "../device/client.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  char status[1024] = {0};
  aula_gateway_response response = {0};
  int result;
  if (argc != 3) return 2;
  result = aula_device_read_status_fd(atoi(argv[1]), (uint32_t)strtoul(argv[2], NULL, 10),
      status, sizeof(status));
  gateway_finish_device_reply(result, status, &response);
  puts(response.body);
  return response.status == 200 ? 0 : 1;
}
