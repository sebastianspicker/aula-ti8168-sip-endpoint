#include "client.h"
#include "projection.h"
#include "transport.h"
#include "wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

json_t *ls200_device_exchange(json_t *request) {
  uint64_t deadline = ls200_device_clock() + 4500;
  json_t *response = NULL;
  int fd = ls200_device_connect(LS200_DEVICE_SOCKET, 0, 500);
  if (fd < 0) return NULL;
  if (ls200_device_send_message(fd, request, deadline)) response = ls200_device_receive_message(fd, deadline);
  close(fd);
  return response;
}

int ls200_device_read_status(char *output, size_t capacity) {
  int fd = ls200_device_connect(LS200_DEVICE_SOCKET, 0, 1000);
  uint64_t deadline = ls200_device_clock() + 4500;
  json_t *request, *response = NULL;
  char *encoded = NULL;
  int result = 0;
  if (fd < 0) return 0;
  request = json_pack("{s:i,s:s}", "revision", 1, "operation", "status");
  if (request != NULL && ls200_device_send_message(fd, request, deadline))
    response = ls200_device_receive_message(fd, deadline);
  json_decref(request);
  close(fd);
  if (ls200_device_status_valid(response)) encoded = json_dumps(response, JSON_COMPACT);
  if (encoded != NULL && strlen(encoded) < capacity) {
    snprintf(output, capacity, "%s", encoded);
    result = 1;
  }
  free(encoded);
  json_decref(response);
  return result;
}
