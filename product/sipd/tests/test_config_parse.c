/* Configuration loading: the strict INI parser behind aula_config_load_file
 * and the pure helpers in config_private.h. Config files are written into a
 * private mkdtemp directory; the shipped example supplies the baseline text
 * with its /run and /var/lib paths redirected into that directory. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "aula_test.h"
#include "core/config_private.h"

#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PATH_BYTES 256U
#define TEST_CONFIG_BYTES 8192U

static char test_directory[TEST_PATH_BYTES];
static char test_config_path[TEST_PATH_BYTES];
static char test_pid_path[TEST_PATH_BYTES];
static char test_settings_path[TEST_PATH_BYTES];

static int make_directory(void) {
  const char *base = getenv("TMPDIR");
  if (base == NULL || base[0] != '/' || strlen(base) > 40U) base = "/tmp";
  if (snprintf(test_directory, sizeof(test_directory), "%s/aula-config-XXXXXX",
               base) >= (int)sizeof(test_directory) ||
      mkdtemp(test_directory) == NULL) return 0;
  return snprintf(test_config_path, sizeof(test_config_path), "%s/aula-sipd.conf",
                  test_directory) < (int)sizeof(test_config_path) &&
         snprintf(test_pid_path, sizeof(test_pid_path), "%s/aula-sipd.pid",
                  test_directory) < (int)sizeof(test_pid_path) &&
         snprintf(test_settings_path, sizeof(test_settings_path),
                  "%s/settings.json", test_directory) <
             (int)sizeof(test_settings_path);
}

/* Removes only the one file this test creates and its private directory. */
static void remove_directory(void) {
  (void)unlink(test_config_path);
  (void)rmdir(test_directory);
}

/* Writes the config at mode 0600, replacing any earlier file. */
static int write_config(const char *text) {
  FILE *file;
  (void)unlink(test_config_path);
  file = fopen(test_config_path, "w");
  if (file == NULL) return 0;
  if (fputs(text, file) == EOF) {
    (void)fclose(file);
    return 0;
  }
  if (fclose(file) != 0) return 0;
  return chmod(test_config_path, 0600) == 0;
}

/* The shipped example, with the two host-specific paths redirected. The
 * placeholders are filled by write_variant(): `leading` goes before the first
 * section header and `injected` at the end of the [runtime] section. */
static const char config_template[] =
    "%s"
    "[runtime]\n"
    "foreground = true\n"
    "pid_file = %s\n"
    "settings_file = %s\n"
    "log_sink = syslog\n"
    "log_level = info\n"
    "log_summary_interval_seconds = 60\n"
    "%s"
    "\n"
    "[sip]\n"
    "profile = private_lab\n"
    "uri =\n"
    "registrar_uri =\n"
    "transport = udp\n"
    "auth_username =\n"
    "auth_secret_file =\n"
    "tls_ca_file =\n"
    "enable_tls = false\n"
    "enable_public_network = false\n"
    "transaction_timeout_ms = 32000\n"
    "max_retransmissions = 6\n"
    "max_reconnect_attempts = 3\n"
    "\n"
    "[media]\n"
    "backend = fixture\n"
    "video_source =\n"
    "audio_source =\n"
    "authorized_rtsp_ipv4 = 127.0.0.1\n"
    "bind_address = 127.0.0.1\n"
    "advertised_address = 127.0.0.1\n"
    "preferred_audio_codec = pcmu\n"
    "security = plain_compat\n"
    "rtsp_use_udp = false\n"
    "rtsp_jitter_max_ms = 120\n"
    "rtsp_reconnect_limit = 3\n"
    "local_rtp_port_min = 40000\n"
    "local_rtp_port_max = 40100\n"
    "rtp_mtu = 1200\n"
    "aec_enabled = false\n"
    "aec_reference_delay_frames = 0\n"
    "aec_delay_calibrated = false\n"
    "video_queue_frames = 8\n"
    "audio_queue_frames = 10\n"
    "child_uid = 65534\n"
    "child_gid = 65534\n"
    "\n"
    "[control]\n"
    "enable_local_control = false\n"
    "unix_socket_path = /run/aula-sipd/control.sock\n"
    "unix_socket_mode = 0660\n"
    "gateway_uid = 65534\n"
    "\n"
    "[limits]\n"
    "sip_message_bytes = 16384\n"
    "sdp_bytes = 8192\n"
    "rtp_packet_bytes = 1500\n"
    "video_access_unit_bytes = 2097152\n"
    "audio_frame_bytes = 4096\n"
    "control_message_bytes = 2048\n"
    "log_events_per_interval = 120\n";

static int write_variant(const char *leading, const char *injected) {
  static char text[TEST_CONFIG_BYTES];
  int length = snprintf(text, sizeof(text), config_template, leading,
                        test_pid_path, test_settings_path, injected);
  return length > 0 && (size_t)length < sizeof(text) && write_config(text);
}

static aula_status load_variant(const char *leading, const char *injected) {
  aula_config *config = NULL;
  aula_status status;
  if (!write_variant(leading, injected)) return AULA_STATUS_IO_ERROR;
  status = aula_config_load_file(test_config_path, &config);
  AULA_CHECK((status == AULA_STATUS_OK) == (config != NULL));
  aula_config_destroy(config);
  return status;
}

static void test_valid_config_loads(void) {
  aula_config *config = NULL;
  const aula_config_view *view;
  AULA_CHECK(write_variant("# comment before the first section\n\n", ""));
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                    AULA_STATUS_OK);
  AULA_CHECK(config != NULL);
  if (config == NULL) return;
  view = aula_config_get_view(config);
  AULA_CHECK(view != NULL);
  if (view != NULL) {
    AULA_CHECK(view->foreground == 1);
    AULA_CHECK(strcmp(view->pid_file, test_pid_path) == 0);
    AULA_CHECK(strcmp(view->runtime_settings_file, test_settings_path) == 0);
    AULA_CHECK(strcmp(view->sip_profile, "private_lab") == 0);
    AULA_CHECK(view->transport == AULA_TRANSPORT_UDP);
    AULA_CHECK(strcmp(view->media_backend, "fixture") == 0);
    AULA_CHECK(strcmp(view->media_bind_address, "127.0.0.1") == 0);
    AULA_CHECK(view->preferred_audio_codec == AULA_G711_PCMU);
    AULA_CHECK(view->media_security_policy == PLAIN_COMPAT);
    AULA_CHECK(view->local_rtp_port_min == 40000U);
    AULA_CHECK(view->local_rtp_port_max == 40100U);
    AULA_CHECK(view->rtp_mtu == 1200U);
    AULA_CHECK(view->enable_local_control == 0);
    AULA_CHECK(view->limits.sdp_bytes == 8192U);
  }
  aula_config_destroy(config);
}

static void test_loader_rejects_bad_syntax(void) {
  /* An unknown key, a duplicate key and an unknown section are policy
   * violations; a structurally malformed line is invalid data. */
  AULA_CHECK_STATUS(load_variant("", "bogus_key = 1\n"),
                    AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK_STATUS(load_variant("", "foreground = true\n"),
                    AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK_STATUS(load_variant("", "\n[bogus]\n"),
                    AULA_STATUS_CONFIGURATION_ERROR);
  AULA_CHECK_STATUS(load_variant("foreground = true\n", ""),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(load_variant("", "foreground\n"),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(load_variant("", "log_level = info = debug\n"),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(load_variant("", "= value\n"), AULA_STATUS_INVALID_DATA);
  /* A parent-directory segment in a value is refused before key lookup. */
  AULA_CHECK_STATUS(load_variant("", "unix_socket_path = /run/../x\n"),
                    AULA_STATUS_INVALID_DATA);
  /* Malformed section headers. */
  AULA_CHECK_STATUS(load_variant("", "\n[]\n"), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(load_variant("", "\n[run time]\n"),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(load_variant("", "\n[runtime\n"),
                    AULA_STATUS_INVALID_DATA);
}

static void test_loader_enforces_size_limits(void) {
  static char long_line[AULA_CONFIG_LINE_BYTES + 64U];
  static char long_value[AULA_CONFIG_VALUE_BYTES + 64U];
  static char huge[AULA_CONFIG_FILE_BYTES + 1024U];
  size_t used;
  /* A line that does not fit the line buffer is a limit error, even when it
   * is only a comment. */
  (void)memset(long_line, 'x', sizeof(long_line) - 2U);
  long_line[0] = '#';
  long_line[sizeof(long_line) - 2U] = '\n';
  long_line[sizeof(long_line) - 1U] = '\0';
  AULA_CHECK_STATUS(load_variant("", long_line), AULA_STATUS_LIMIT_EXCEEDED);
  /* A value at the per-value cap is refused as invalid data. */
  (void)memcpy(long_value, "sip_message_bytes = ", 20U);
  (void)memset(long_value + 20, '1', AULA_CONFIG_VALUE_BYTES);
  long_value[20U + AULA_CONFIG_VALUE_BYTES] = '\n';
  long_value[21U + AULA_CONFIG_VALUE_BYTES] = '\0';
  AULA_CHECK_STATUS(load_variant("", long_value), AULA_STATUS_INVALID_DATA);
  /* The whole file is capped; comment lines alone can exceed it. */
  for (used = 0U; used + 80U < sizeof(huge) - 1U; used += 80U) {
    (void)memset(huge + used, '#', 79U);
    huge[used + 79U] = '\n';
  }
  huge[used] = '\0';
  {
    aula_config *config = NULL;
    AULA_CHECK(write_config(huge));
    AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                      AULA_STATUS_LIMIT_EXCEEDED);
    AULA_CHECK(config == NULL);
  }
}

static void test_loader_refuses_unsafe_files(void) {
  aula_config *config = NULL;
  AULA_CHECK(write_variant("", ""));
  AULA_CHECK(chmod(test_config_path, 0660) == 0);
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                    AULA_STATUS_PERMISSION_DENIED);
  AULA_CHECK(config == NULL);
  AULA_CHECK(chmod(test_config_path, 0602) == 0);
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                    AULA_STATUS_PERMISSION_DENIED);
  AULA_CHECK(chmod(test_config_path, 0600) == 0);
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                    AULA_STATUS_OK);
  AULA_CHECK(config != NULL);
  /* An output slot that is already populated is refused. */
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, &config),
                    AULA_STATUS_INVALID_ARGUMENT);
  aula_config_destroy(config);
  config = NULL;
  AULA_CHECK_STATUS(aula_config_load_file(NULL, &config),
                    AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK_STATUS(aula_config_load_file(test_config_path, NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
}

static void test_trim(void) {
  char padded[] = " \t key = value \r\n";
  char clean[] = "word";
  char blank[] = " \t\r\n";
  char empty[] = "";
  AULA_CHECK(strcmp(aula_config_trim(padded), "key = value") == 0);
  AULA_CHECK(aula_config_trim(clean) == clean);
  AULA_CHECK(strcmp(aula_config_trim(clean), "word") == 0);
  AULA_CHECK(aula_config_trim(blank)[0] == '\0');
  AULA_CHECK(aula_config_trim(empty)[0] == '\0');
}

static void test_ipv4_literal_parsing(void) {
  uint8_t octets[4] = {0U, 0U, 0U, 0U};
  AULA_CHECK(aula_config_parse_ipv4_literal("192.168.1.20", octets) == 1);
  AULA_CHECK(octets[0] == 192U && octets[1] == 168U && octets[2] == 1U &&
             octets[3] == 20U);
  AULA_CHECK(aula_config_parse_ipv4_literal("0.0.0.0", octets) == 1);
  AULA_CHECK(aula_config_parse_ipv4_literal("255.255.255.255", octets) == 1);
  AULA_CHECK(octets[0] == 255U && octets[3] == 255U);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3.0", octets) == 1);
  AULA_CHECK(aula_config_parse_ipv4_literal(NULL, octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("127.0.0.1", NULL) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("256.0.0.1", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3.4.5", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3.4 ", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3.", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1..3.4", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("a.b.c.d", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1000.0.0.1", octets) == 0);
  /* Only the canonical form is accepted: no leading zeros. */
  AULA_CHECK(aula_config_parse_ipv4_literal("01.2.3.4", octets) == 0);
  AULA_CHECK(aula_config_parse_ipv4_literal("1.2.3.004", octets) == 0);
}

static void test_ipv4_classification(void) {
  static const uint8_t loopback[4] = {127U, 1U, 2U, 3U};
  static const uint8_t ten[4] = {10U, 0U, 0U, 1U};
  static const uint8_t private_low[4] = {172U, 16U, 0U, 1U};
  static const uint8_t private_high[4] = {172U, 31U, 255U, 255U};
  static const uint8_t private_below[4] = {172U, 15U, 0U, 1U};
  static const uint8_t private_above[4] = {172U, 32U, 0U, 1U};
  static const uint8_t lan[4] = {192U, 168U, 0U, 9U};
  static const uint8_t lan_other[4] = {192U, 169U, 0U, 9U};
  static const uint8_t link_local[4] = {169U, 254U, 1U, 1U};
  static const uint8_t public_address[4] = {8U, 8U, 8U, 8U};
  static const uint8_t unspecified[4] = {0U, 0U, 0U, 0U};
  static const uint8_t broadcast[4] = {255U, 255U, 255U, 255U};
  AULA_CHECK(aula_config_ipv4_is_loopback(loopback) == 1);
  AULA_CHECK(aula_config_ipv4_is_loopback(ten) == 0);
  AULA_CHECK(aula_config_ipv4_is_loopback(public_address) == 0);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(loopback) == 1);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(ten) == 1);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(private_low) == 1);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(private_high) == 1);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(private_below) == 0);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(private_above) == 0);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(lan) == 1);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(lan_other) == 0);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(link_local) == 0);
  AULA_CHECK(aula_config_ipv4_is_private_or_loopback(public_address) == 0);
  /* RTSP targets additionally allow link-local but never the unspecified or
   * broadcast address. */
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(loopback) == 1);
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(lan) == 1);
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(link_local) == 1);
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(public_address) == 0);
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(unspecified) == 0);
  AULA_CHECK(aula_config_ipv4_is_authorized_rtsp_target(broadcast) == 0);
}

static void test_control_or_parent_segment(void) {
  AULA_CHECK(aula_config_has_control_or_parent_segment("") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/aula.pid") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/..") == 1);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/../etc") == 1);
  AULA_CHECK(aula_config_has_control_or_parent_segment("../etc") == 1);
  AULA_CHECK(aula_config_has_control_or_parent_segment("..") == 1);
  /* Dots inside a longer segment are ordinary file name characters. */
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/..x/a") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/x../a") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/.../a") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("/run/./a") == 0);
  AULA_CHECK(aula_config_has_control_or_parent_segment("a\nb") == 1);
  AULA_CHECK(aula_config_has_control_or_parent_segment("a\tb") == 1);
  AULA_CHECK(aula_config_has_control_or_parent_segment("a\x7f") == 1);
}

int main(void) {
  if (!make_directory()) {
    (void)fprintf(stderr, "test_config_parse: cannot create a test directory\n");
    return EXIT_FAILURE;
  }
  test_valid_config_loads();
  test_loader_rejects_bad_syntax();
  test_loader_enforces_size_limits();
  test_loader_refuses_unsafe_files();
  test_trim();
  test_ipv4_literal_parsing();
  test_ipv4_classification();
  test_control_or_parent_segment();
  remove_directory();
  return aula_test_result("test_config_parse");
}
