#define _POSIX_C_SOURCE 200809L

#include "core/config_route.h"
#include "sip/crc_route.h"
#include "media/receive_monitor.h"
#include "ls200_sipd/endpoint.h"
#include "ls200_sipd/ls200_sipd.h"
#include "ls200_sipd/media_renderer.h"
#include "ls200_sipd/pjsip.h"
#include "ls200_sipd/platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { EXIT_USAGE = 2, EXIT_CONFIG = 3, EXIT_RUNTIME = 4, EXIT_SELF_TEST = 5 };

static ls200_status foreground_credential_provider(
    void *context, const ls200_sip_digest_challenge *challenge,
    ls200_mutable_bytes *out_response) {
  const char *path = (const char *)context;
  (void)challenge;
  if (path == NULL || path[0] == '\0') return LS200_STATUS_PERMISSION_DENIED;
  return ls200_config_read_secret_file(path, out_response);
}

static void usage(FILE *stream) {
  (void)fputs("usage: ls200-sipd --version | --check-config PATH | --self-test | --fixture-call | --foreground (--config PATH | --config-fd FD)\n", stream);
}

static int self_test(void) {
  ls200_call *call = NULL;
  ls200_call_transition transition;
  uint8_t random_data[16];
  ls200_mutable_bytes random_output;
  random_output.data = random_data;
  random_output.capacity = sizeof(random_data);
  random_output.length = 0U;
  if (ls200_platform_monotonic_now(&(uint64_t){0U}) != LS200_STATUS_OK ||
      ls200_platform_random_bytes(&random_output) != LS200_STATUS_OK || random_output.length != sizeof(random_data) ||
      ls200_call_create(&call) != LS200_STATUS_OK ||
      ls200_call_apply_event(call, LS200_CALL_EVENT_START, &transition) != LS200_STATUS_OK ||
      transition.to != LS200_CALL_RESOLVING ||
      ls200_call_apply_event(call, LS200_CALL_EVENT_SHUTDOWN, NULL) != LS200_STATUS_OK ||
      ls200_call_get_state(call) != LS200_CALL_STOPPED) {
    ls200_call_destroy(call);
    return EXIT_SELF_TEST;
  }
  ls200_call_destroy(call);
  (void)puts("self-test: ok");
  return 0;
}

static int fixture_call(void) {
  static const struct fixture_step {
    ls200_call_event event;
    ls200_call_state expected_state;
  } steps[] = {
    {LS200_CALL_EVENT_START, LS200_CALL_RESOLVING},
    {LS200_CALL_EVENT_RESOLVED, LS200_CALL_INVITING},
    {LS200_CALL_EVENT_PROVISIONAL, LS200_CALL_EARLY},
    {LS200_CALL_EVENT_ACCEPTED, LS200_CALL_ESTABLISHING_MEDIA},
    {LS200_CALL_EVENT_MEDIA_READY, LS200_CALL_ESTABLISHED},
    {LS200_CALL_EVENT_LOCAL_HANGUP, LS200_CALL_TERMINATING},
    {LS200_CALL_EVENT_TERMINATION_COMPLETE, LS200_CALL_TERMINATED},
    {LS200_CALL_EVENT_SHUTDOWN, LS200_CALL_STOPPED}
  };
  ls200_call *call = NULL;
  size_t index;
  if (ls200_call_create(&call) != LS200_STATUS_OK) return EXIT_SELF_TEST;
  for (index = 0U; index < sizeof(steps) / sizeof(steps[0]); ++index) {
    ls200_call_transition transition;
    if (ls200_call_apply_event(call, steps[index].event, &transition) != LS200_STATUS_OK ||
        transition.to != steps[index].expected_state ||
        ls200_call_get_state(call) != steps[index].expected_state) {
      ls200_call_destroy(call);
      return EXIT_SELF_TEST;
    }
  }
  ls200_call_destroy(call);
  (void)puts("fixture-call: ok network=disabled media_io=disabled rx_rendering=false");
  return 0;
}

static ls200_zoom_profile profile_from_name(const char *profile) {
  if (strcmp(profile, "zoom_direct") == 0) return LS200_ZOOM_PROFILE_DIRECT_CRC;
  if (strcmp(profile, "zoom_proxy") == 0) return LS200_ZOOM_PROFILE_PROXY_REGISTRATION;
  return LS200_ZOOM_PROFILE_PRIVATE_LAB;
}

static ls200_status create_foreground_renderer(const ls200_config_view *view,
                                               ls200_media_renderer *renderer) {
  const char *monitor_path = getenv("LS200_SIPD_RECEIVE_MONITOR");
  if (strcmp(view->media_backend, "fixture") == 0) {
    return ls200_fake_renderer_create(
        LS200_MEDIA_RENDER_G711_RX | LS200_MEDIA_RENDER_H264_RX |
        LS200_MEDIA_RENDER_FAKE_AUDIO | LS200_MEDIA_RENDER_FAKE_HDMI, renderer);
  }
  if (monitor_path != NULL) return ls200_receive_monitor_create(monitor_path, renderer);
  return ls200_vendor_arec_renderer_create(renderer);
}

static int poll_foreground_endpoint(ls200_endpoint *endpoint, ls200_status *out_status) {
  for (;;) {
    uint64_t now;
    ls200_deadline deadline;
    if (ls200_platform_monotonic_now(&now) != LS200_STATUS_OK) return EXIT_RUNTIME;
    deadline.monotonic_ns = UINT64_MAX - now < UINT64_C(20000000)
        ? UINT64_MAX : now + UINT64_C(20000000);
    *out_status = ls200_endpoint_poll(endpoint, deadline);
    if (*out_status == LS200_STATUS_END) return 0;
    if (*out_status != LS200_STATUS_OK && *out_status != LS200_STATUS_AGAIN)
      return EXIT_RUNTIME;
  }
}

static int foreground_runtime_config(ls200_config *config) {
  const ls200_config_view *view;
  ls200_pjsip_driver_options pjsip_options;
  ls200_sip_driver_config driver_config;
  ls200_endpoint_options endpoint_options;
  ls200_endpoint *endpoint = NULL;
  ls200_media_renderer renderer;
  ls200_status status;
  int renderer_created = 0;
  int result = EXIT_RUNTIME;
  view = ls200_config_get_view(config);
  if (view == NULL || !view->foreground) { ls200_config_destroy(config); return EXIT_CONFIG; }
  if (!ls200_pjsip_driver_is_available()) {
    (void)fprintf(stderr, "foreground startup refused: no approved SIP driver is linked\n");
    ls200_config_destroy(config);
    return EXIT_RUNTIME;
  }
  (void)memset(&pjsip_options, 0, sizeof(pjsip_options));
  (void)memset(&driver_config, 0, sizeof(driver_config));
  (void)memset(&endpoint_options, 0, sizeof(endpoint_options));
  (void)memset(&renderer, 0, sizeof(renderer));
  pjsip_options.profile = profile_from_name(view->sip_profile);
  pjsip_options.registrar_uri = view->sip_registrar_uri[0] == '\0'
      ? NULL : view->sip_registrar_uri;
  pjsip_options.local_uri = view->sip_uri;
  pjsip_options.tls_ca_file = view->tls_ca_file[0] == '\0'
      ? NULL : view->tls_ca_file;
  status = ls200_pjsip_driver_configure_route(&pjsip_options,
      ls200_config_crc_address(config), &driver_config);
  if (status != LS200_STATUS_OK) goto cleanup;
  endpoint_options.sip_driver = &driver_config;
  endpoint_options.credential_provider = foreground_credential_provider;
  endpoint_options.credential_context = (void *)view->auth_secret_file;
  status = create_foreground_renderer(view, &renderer);
  if (status == LS200_STATUS_OK) {
    endpoint_options.renderer = &renderer;
    renderer_created = 1;
  } else if (status != LS200_STATUS_UNSUPPORTED) {
    goto cleanup;
  }
  status = ls200_endpoint_create(config, &endpoint_options, &endpoint);
  if (status != LS200_STATUS_OK ||
      ls200_platform_install_signal_handlers() != LS200_STATUS_OK) goto cleanup;
  result = poll_foreground_endpoint(endpoint, &status);
cleanup:
  if (result != 0)
    (void)fprintf(stderr, "foreground startup failed: status=%d\n", (int)status);
  ls200_endpoint_destroy(endpoint);
  if (renderer_created != 0 && renderer.vtable != NULL &&
      renderer.vtable->destroy != NULL) renderer.vtable->destroy(&renderer);
  ls200_config_destroy(config);
  return result;
}

static int foreground_runtime(const char *path) {
  ls200_config *config = NULL;
  if (ls200_config_load_file(path, &config) != LS200_STATUS_OK)
    return EXIT_CONFIG;
  return foreground_runtime_config(config);
}

static int foreground_runtime_fd(const char *value) {
  char *end = NULL;
  const char *cursor;
  unsigned long parsed;
  ls200_config *config = NULL;
  if (value == NULL || value[0] == '\0') return EXIT_CONFIG;
  for (cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return EXIT_CONFIG;
  }
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > INT_MAX ||
      ls200_config_load_fd((int)parsed, &config) != LS200_STATUS_OK)
    return EXIT_CONFIG;
  return foreground_runtime_config(config);
}

static int command_matches(int argc, char **argv, int expected_argc,
                           const char *first, const char *second) {
  if (argc != expected_argc || strcmp(argv[1], first) != 0) return 0;
  if (second != NULL && strcmp(argv[2], second) != 0) return 0;
  return 1;
}

static int check_config(const char *path) {
  ls200_config *config = NULL;
  ls200_status status = ls200_config_load_file(path, &config);
  if (status != LS200_STATUS_OK) return EXIT_CONFIG;
  ls200_config_destroy(config);
  (void)puts("configuration: valid");
  return 0;
}

int main(int argc, char **argv) {
  if (command_matches(argc, argv, 2, "--version", NULL)) {
    (void)printf("ls200-sipd %u.%u.%u\n", LS200_SIPD_VERSION_MAJOR, LS200_SIPD_VERSION_MINOR, LS200_SIPD_VERSION_PATCH);
    return 0;
  }
  if (command_matches(argc, argv, 2, "--self-test", NULL)) return self_test();
  if (command_matches(argc, argv, 2, "--fixture-call", NULL)) return fixture_call();
  if (command_matches(argc, argv, 3, "--check-config", NULL)) return check_config(argv[2]);
  if (command_matches(argc, argv, 3, "--foreground", "--config")) {
    usage(stderr);
    return EXIT_USAGE;
  }
  if (command_matches(argc, argv, 4, "--foreground", "--config"))
    return foreground_runtime(argv[3]);
  if (command_matches(argc, argv, 4, "--foreground", "--config-fd"))
    return foreground_runtime_fd(argv[3]);
  usage(stderr);
  return EXIT_USAGE;
}
