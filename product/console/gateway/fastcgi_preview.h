#ifndef AULA_CONSOLE_FASTCGI_PREVIEW_H
#define AULA_CONSOLE_FASTCGI_PREVIEW_H

#include "gateway.h"

#include <fcgiapp.h>
#include <pthread.h>

typedef struct aula_fastcgi_server {
  aula_gateway gateway;
  pthread_mutex_t preview_mutex;
  int preview_busy;
  int listener;
} aula_fastcgi_server;

int aula_fastcgi_is_preview_path(const char *path);
void aula_fastcgi_handle_preview(FCGX_Request *request,
                                  aula_fastcgi_server *server,
                                  const aula_gateway_request *api_request);

#endif
