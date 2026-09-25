#include "wire.h"
#include "transport.h"
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

json_t *ls200_device_receive_message(int fd, uint64_t deadline) {
  uint32_t size;
  char buffer[LS200_DEVICE_LIMIT];
  json_error_t error;
  if (!ls200_device_transfer(fd, &size, sizeof(size), 0, deadline)) return NULL;
  size = ntohl(size);
  if (size == 0 || size > sizeof(buffer) ||
      !ls200_device_transfer(fd, buffer, size, 0, deadline)) return NULL;
  json_t *message = json_loadb(buffer, size, JSON_REJECT_DUPLICATES, &error);
  OPENSSL_cleanse(buffer, sizeof(buffer));
  return message;
}

int ls200_device_send_message(int fd, json_t *message, uint64_t deadline) {
  char *encoded = json_dumps(message, JSON_COMPACT | JSON_SORT_KEYS);
  uint32_t size;
  int result = 0;
  if (encoded == NULL) return 0;
  if (strlen(encoded) <= LS200_DEVICE_LIMIT) {
    size = htonl((uint32_t)strlen(encoded));
    result = ls200_device_transfer(fd, &size, sizeof(size), 1, deadline) &&
        ls200_device_transfer(fd, encoded, strlen(encoded), 1, deadline);
  }
  OPENSSL_cleanse(encoded, strlen(encoded));
  free(encoded);
  return result;
}
