#ifndef LS200_CONSOLE_FASTCGI_PREVIEW_H
#define LS200_CONSOLE_FASTCGI_PREVIEW_H

#include "gateway.h"

#include <fcgiapp.h>
#include <pthread.h>

typedef struct ls200_fastcgi_server {
  ls200_gateway gateway;
  pthread_mutex_t preview_mutex;
  int preview_busy;
  int listener;
} ls200_fastcgi_server;

int ls200_fastcgi_is_preview_path(const char *path);
void ls200_fastcgi_handle_preview(FCGX_Request *request,
                                  ls200_fastcgi_server *server,
                                  const ls200_gateway_request *api_request);

#endif
