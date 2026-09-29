#ifndef AULA_SIPD_RESOLVER_INTERNAL_H
#define AULA_SIPD_RESOLVER_INTERNAL_H

#include "aula_sipd/common.h"

#include <stddef.h>
#include <stdint.h>

#define AULA_SIPD_DNS_SERVER_LIMIT 3U

typedef struct aula_sip_dns_servers {
  uint8_t addresses[AULA_SIPD_DNS_SERVER_LIMIT][4];
  size_t count;
} aula_sip_dns_servers;

aula_status aula_sip_dns_servers_parse(const char *value,
                                         aula_sip_dns_servers *out_servers);
aula_status aula_sip_resolver_configure(const char *dns_servers);
aula_status aula_sip_resolver_prepare(void);

#endif
