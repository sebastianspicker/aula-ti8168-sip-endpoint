#ifndef LS200_CONSOLE_FASTCGI_REQUEST_H
#define LS200_CONSOLE_FASTCGI_REQUEST_H

#include "gateway.h"

/* Translate the fixed nginx FastCGI environment without retaining or copying
 * attacker-controlled values. Values remain owned by envp for the request
 * lifetime. Duplicate, non-canonical, queried, or oversized inputs fail
 * before gateway route dispatch. */
int ls200_fastcgi_request_map(char *const envp[], const char *body, uint64_t now,
                              ls200_gateway_request *out_request);

#endif
