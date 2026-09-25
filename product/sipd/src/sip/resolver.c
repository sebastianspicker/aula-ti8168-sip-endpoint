#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "resolver_internal.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__GLIBC__)
#include <resolv.h>
#endif

#define LS200_SIPD_DNS_SERVERS_ENV "LS200_SIPD_DNS_SERVERS"
#define LS200_SIPD_IPV4_TEXT_BYTES 16U
#define LS200_SIPD_DNS_SERVERS_TEXT_LIMIT \
  ((LS200_SIPD_DNS_SERVER_LIMIT * (LS200_SIPD_IPV4_TEXT_BYTES - 1U)) + \
   (LS200_SIPD_DNS_SERVER_LIMIT - 1U))

static size_t resolver_bounded_length(const char *value, size_t limit) {
  size_t length = 0U;
  while (length <= limit && value[length] != '\0') ++length;
  return length;
}

ls200_status ls200_sip_dns_servers_parse(
    const char *value, ls200_sip_dns_servers *out_servers) {
  const char *cursor;
  size_t value_length;
  if (value == NULL || out_servers == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(out_servers, 0, sizeof(*out_servers));
  value_length = resolver_bounded_length(value, LS200_SIPD_DNS_SERVERS_TEXT_LIMIT);
  if (value_length == 0U) return LS200_STATUS_INVALID_DATA;
  if (value_length > LS200_SIPD_DNS_SERVERS_TEXT_LIMIT)
    return LS200_STATUS_LIMIT_EXCEEDED;

  cursor = value;
  while (*cursor != '\0') {
    const char *separator = strchr(cursor, ',');
    size_t token_length = separator == NULL ? strlen(cursor) :
                                               (size_t)(separator - cursor);
    char token[LS200_SIPD_IPV4_TEXT_BYTES];
    if (out_servers->count >= LS200_SIPD_DNS_SERVER_LIMIT)
      return LS200_STATUS_LIMIT_EXCEEDED;
    if (token_length == 0U || token_length >= sizeof(token))
      return LS200_STATUS_INVALID_DATA;
    (void)memcpy(token, cursor, token_length);
    token[token_length] = '\0';
    if (inet_pton(AF_INET, token, out_servers->addresses[out_servers->count]) != 1)
      return LS200_STATUS_INVALID_DATA;
    ++out_servers->count;
    if (separator == NULL) break;
    cursor = separator + 1U;
    if (*cursor == '\0') return LS200_STATUS_INVALID_DATA;
  }
  return LS200_STATUS_OK;
}

ls200_status ls200_sip_resolver_configure(const char *dns_servers) {
  ls200_sip_dns_servers parsed;
  ls200_status status;
  size_t index;
  if (dns_servers == NULL) return LS200_STATUS_OK;
  status = ls200_sip_dns_servers_parse(dns_servers, &parsed);
  if (status != LS200_STATUS_OK) return status;
#if (defined(__linux__) && defined(__GLIBC__)) || \
    defined(LS200_SIPD_RESOLVER_TEST_STUB)
  if (parsed.count > (size_t)MAXNS) return LS200_STATUS_LIMIT_EXCEEDED;
  if (res_init() != 0) return LS200_STATUS_IO_ERROR;
  /* Close inherited resolver sockets before replacing the public IPv4 list.
   * glibc owns the private extension metadata used to map those servers. */
  res_close();
  (void)memset(_res.nsaddr_list, 0, sizeof(_res.nsaddr_list));
  _res.nscount = (int)parsed.count;
  for (index = 0U; index < parsed.count; ++index) {
    _res.nsaddr_list[index].sin_family = AF_INET;
    _res.nsaddr_list[index].sin_port =
        (in_port_t)htons((uint16_t)53U);
    (void)memcpy(&_res.nsaddr_list[index].sin_addr, parsed.addresses[index],
                 sizeof(parsed.addresses[index]));
  }
  _res.options |= RES_INIT;
  return LS200_STATUS_OK;
#else
  (void)index;
  return LS200_STATUS_UNSUPPORTED;
#endif
}

ls200_status ls200_sip_resolver_prepare(void) {
  return ls200_sip_resolver_configure(getenv(LS200_SIPD_DNS_SERVERS_ENV));
}
