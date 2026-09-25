#ifndef LS200_SIPD_RESOLVER_INTERNAL_H
#define LS200_SIPD_RESOLVER_INTERNAL_H

#include "ls200_sipd/common.h"

#include <stddef.h>
#include <stdint.h>

#define LS200_SIPD_DNS_SERVER_LIMIT 3U

typedef struct ls200_sip_dns_servers {
  uint8_t addresses[LS200_SIPD_DNS_SERVER_LIMIT][4];
  size_t count;
} ls200_sip_dns_servers;

ls200_status ls200_sip_dns_servers_parse(const char *value,
                                         ls200_sip_dns_servers *out_servers);
ls200_status ls200_sip_resolver_configure(const char *dns_servers);
ls200_status ls200_sip_resolver_prepare(void);

#endif
