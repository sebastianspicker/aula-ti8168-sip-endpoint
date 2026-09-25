/* Optional production FastCGI adapter.  Host tests deliberately compile the
 * pure gateway without this translation unit when FastCGI is unavailable. */
#ifdef LS200_GATEWAY_WITH_FCGI
#define _POSIX_C_SOURCE 200809L
#include "gateway.h"
#include "fastcgi_preview.h"
#include "fastcgi_request.h"
#include <fcgiapp.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef LS200_GATEWAY_ALLOWED_ORIGIN
#define LS200_GATEWAY_ALLOWED_ORIGIN "https://console.invalid:8443"
#endif
#ifndef LS200_GATEWAY_CONTROL_SOCKET
#define LS200_GATEWAY_CONTROL_SOCKET "/run/ls200-sipd/control.sock"
#endif
#ifndef LS200_GATEWAY_ACCOUNT_STORE
#define LS200_GATEWAY_ACCOUNT_STORE "/var/lib/ls200-console/account.json"
#endif
#ifndef LS200_GATEWAY_POLICY
#define LS200_GATEWAY_POLICY "/etc/ls200-console/gateway.conf"
#endif
#define LS200_GATEWAY_WORKERS 4U

static unsigned int status_reason(unsigned int status) {
  return status == 200U ? 200U : status;
}

static int body_from_request(FCGX_Request *request, char body[1025]) {
  const char *length_text = FCGX_GetParam("CONTENT_LENGTH", request->envp);
  size_t length;
  if (!ls200_gateway_parse_content_length(length_text, 1024U, &length)) return 0;
  if (length != 0U && FCGX_GetStr(body, (int)length, request->in) != (int)length) return 0;
  body[length] = '\0';
  return 1;
}

static int listener_path_is_valid(const char *path) {
  return path != NULL && path[0] == '/' && strlen(path) < 256U &&
      strstr(path, "/../") == NULL;
}

static int listener_parent_is_safe(char parent[256], const char *path) {
  char *slash;
  struct stat details;
  (void)snprintf(parent, 256U, "%s", path);
  slash = strrchr(parent, '/');
  if (slash == NULL || slash == parent || slash[1] == '\0') return 0;
  *slash = '\0';
  return lstat(parent, &details) == 0 && S_ISDIR(details.st_mode) &&
      details.st_uid == geteuid() && (details.st_mode & 0022U) == 0U;
}

static int remove_existing_listener(const char *path) {
  struct stat existing;
  if (lstat(path, &existing) != 0) return errno == ENOENT;
  if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid() ||
      existing.st_nlink != 1) return 0;
  return unlink(path) == 0;
}

static int listener_path_is_safe(const char *path) {
  char parent[256];
  return listener_path_is_valid(path) && listener_parent_is_safe(parent, path) &&
      remove_existing_listener(path);
}

static void handle_fastcgi_request(FCGX_Request *request,
                                   ls200_fastcgi_server *server) {
  char body[1025] = {0};
  ls200_gateway_response response = {0};
  ls200_gateway_request api_request = {0};
  uint64_t now;
  if (!body_from_request(request, body)) {
    FCGX_FPrintF(request->out, "Status: 400 Bad Request\r\nContent-Type: application/json\r\n\r\n{\"revision\":1,\"ok\":false,\"error\":{\"code\":\"BAD_JSON\",\"message\":\"body is too large or incomplete\"}}");
    goto cleanup;
  }
  now = (uint64_t)time(NULL);
  if (!ls200_fastcgi_request_map(request->envp, body, now, &api_request)) {
    FCGX_FPrintF(request->out, "Status: 400 Bad Request\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n\r\n{\"revision\":1,\"ok\":false,\"error\":{\"code\":\"REQUEST_INVALID\",\"message\":\"FastCGI request parameters are invalid\"}}");
    goto cleanup;
  }
  if (ls200_fastcgi_is_preview_path(api_request.path)) {
    ls200_fastcgi_handle_preview(request, server, &api_request);
    goto cleanup;
  }
  if (!ls200_gateway_handle(&server->gateway, &api_request,
                            ls200_gateway_control_exchange,
                            &server->gateway.config, &response)) {
    FCGX_FPrintF(request->out, "Status: 500 Internal Server Error\r\nContent-Type: application/json\r\n\r\n{\"revision\":1,\"ok\":false,\"error\":{\"code\":\"INTERNAL\",\"message\":\"request setup failed\"}}");
    goto cleanup;
  }
  FCGX_FPrintF(request->out, "Status: %u\r\nContent-Type: %s\r\n", status_reason(response.status), response.content_type);
  if (response.set_cookie[0] != '\0') FCGX_FPrintF(request->out, "Set-Cookie: %s\r\n", response.set_cookie);
  if (response.retry_after != 0U) FCGX_FPrintF(request->out, "Retry-After: %u\r\n", response.retry_after);
  FCGX_FPrintF(request->out, "Cache-Control: no-store\r\n\r\n%s", response.body);
cleanup:
  OPENSSL_cleanse(&response, sizeof(response));
  OPENSSL_cleanse(&api_request, sizeof(api_request));
  OPENSSL_cleanse(body, sizeof(body));
}

static void *worker_main(void *context) {
  ls200_fastcgi_server *server = (ls200_fastcgi_server *)context;
  FCGX_Request request;
  int accept_result;
  if (FCGX_InitRequest(&request, server->listener, 0) != 0) return NULL;
  for (;;) {
    errno = 0;
    accept_result = FCGX_Accept_r(&request);
    if (accept_result == 0) {
      handle_fastcgi_request(&request, server);
      FCGX_Finish_r(&request);
      continue;
    }
    if (errno == EINTR) continue;
    (void)fprintf(stderr,
                  "gateway runtime: FastCGI accept failed: result=%d errno=%d\n",
                  accept_result, errno);
    return NULL;
  }
}

static int start_workers(ls200_fastcgi_server *server) {
  pthread_t workers[LS200_GATEWAY_WORKERS];
  size_t index;
  for (index = 0U; index < LS200_GATEWAY_WORKERS; ++index) {
    if (pthread_create(&workers[index], NULL, worker_main, server) != 0) {
      (void)fputs("gateway startup: worker creation failed\n", stderr);
      return 0;
    }
  }
  for (index = 0U; index < LS200_GATEWAY_WORKERS; ++index) {
    if (pthread_join(workers[index], NULL) != 0) return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  ls200_fastcgi_server server;
  ls200_gateway_config config = {
    .allowed_origin = LS200_GATEWAY_ALLOWED_ORIGIN,
    .control_socket_path = LS200_GATEWAY_CONTROL_SOCKET,
    .account_store_path = LS200_GATEWAY_ACCOUNT_STORE,
    .policy_path = LS200_GATEWAY_POLICY,
    .bootstrap_code = NULL,
    .max_sessions = LS200_GATEWAY_MAX_SESSIONS
  };
  (void)memset(&server, 0, sizeof(server));
  if (argc != 5 || strcmp(argv[1], "--policy") != 0 ||
      strcmp(argv[3], "--listen") != 0 || argv[2][0] != '/' ||
      strstr(argv[2], "/../") != NULL || !listener_path_is_safe(argv[4])) {
    (void)fputs("gateway startup: invalid fixed arguments or listener path\n", stderr);
    return 64;
  }
  config.policy_path = argv[2];
  ls200_gateway_init(&server.gateway, &config);
  if (!ls200_gateway_load_policy(&server.gateway)) {
    (void)fputs("gateway startup: policy validation failed\n", stderr);
    return 78;
  }
  if (!ls200_gateway_load_account(&server.gateway)) {
    (void)fputs("gateway startup: account-store validation failed\n", stderr);
    return 78;
  }
  if (FCGX_Init() != 0) {
    (void)fputs("gateway startup: FastCGI initialization failed\n", stderr);
    return 70;
  }
  (void)umask(0007);
  server.listener = FCGX_OpenSocket(argv[4], 16);
  if (server.listener < 0 || chmod(argv[4], 0660) != 0) {
    (void)fputs("gateway startup: FastCGI listener creation failed\n", stderr);
    return 71;
  }
  if (pthread_mutex_init(&server.preview_mutex, NULL) != 0) {
    (void)fputs("gateway startup: preview lock initialization failed\n", stderr);
    return 70;
  }
  return start_workers(&server) ? 0 : 72;
}
#else
typedef int ls200_gateway_fastcgi_adapter_not_built;
#endif
