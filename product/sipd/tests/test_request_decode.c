/* LSZ1 request payload decoders: the strict JSON subset the gateway may send
 * for each opcode. Every decoder must accept the documented shape and reject
 * anything else without leaving partial output behind. */
#include "aula_test.h"
#include "control/request_decode.h"

#include <stdint.h>

static aula_bytes text_bytes(const char *text) {
  aula_bytes bytes;
  bytes.data = (const uint8_t *)text;
  bytes.length = strlen(text);
  return bytes;
}

static void test_empty_object(void) {
  aula_bytes null_empty = {NULL, 0U};
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("{}")) == 1);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes(" { } ")) == 1);
  /* A zero-length payload is treated as the empty object. */
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("")) == 1);
  AULA_CHECK(aula_control_payload_is_empty_object(null_empty) == 1);
  AULA_CHECK(aula_control_payload_is_empty_object(
      text_bytes("{\"a\":\"b\"}")) == 0);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("{")) == 0);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("}")) == 0);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("{}x")) == 0);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("[]")) == 0);
  AULA_CHECK(aula_control_payload_is_empty_object(text_bytes("null")) == 0);
}

static void test_originate_accepts_full_object(void) {
  static const char payload[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":\"123456789\",\"passcode\":\"abc123\","
      "\"host_key\":\"\",\"dial_code\":\"\"}";
  aula_control_originate_request request;
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(payload), &request),
                    AULA_STATUS_OK);
  AULA_CHECK(request.dial.profile == AULA_ZOOM_PROFILE_DIRECT_CRC);
  AULA_CHECK(request.dial.layout == AULA_ZOOM_LAYOUT_GALLERY);
  AULA_CHECK(strcmp(request.dial.meeting_id, "123456789") == 0);
  AULA_CHECK(request.dial.meeting_id == request.meeting_id);
  AULA_CHECK(request.dial.passcode != NULL &&
             strcmp(request.dial.passcode, "abc123") == 0);
  /* Empty optional fields are bound as absent. */
  AULA_CHECK(request.dial.host_key == NULL);
  AULA_CHECK(request.dial.dial_code == NULL);
  AULA_CHECK(request.dial.target_domain == NULL);
  AULA_CHECK(request.dial.has_target_port == 0);
}

static void test_originate_profiles_and_layouts(void) {
  aula_control_originate_request request;
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(
      "{\"profile\":\"zoom_proxy\",\"layout\":\"full_screen\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"654321\","
      "\"dial_code\":\"2001\"}"), &request), AULA_STATUS_OK);
  AULA_CHECK(request.dial.profile == AULA_ZOOM_PROFILE_PROXY_REGISTRATION);
  AULA_CHECK(request.dial.layout == AULA_ZOOM_LAYOUT_FULL_SCREEN);
  AULA_CHECK(request.dial.passcode == NULL);
  AULA_CHECK(strcmp(request.dial.host_key, "654321") == 0);
  AULA_CHECK(strcmp(request.dial.dial_code, "2001") == 0);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(
      "{\"profile\":\"private_lab\",\"layout\":\"dual_video\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"\","
      "\"dial_code\":\"\"}"), &request), AULA_STATUS_OK);
  AULA_CHECK(request.dial.profile == AULA_ZOOM_PROFILE_PRIVATE_LAB);
  AULA_CHECK(request.dial.layout == AULA_ZOOM_LAYOUT_DUAL_VIDEO);
}

static void test_originate_rejects_bad_payloads(void) {
  aula_control_originate_request request;
  static const char missing_member[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"\"}";
  static const char unknown_member[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"\","
      "\"bogus\":\"\"}";
  static const char bad_profile[] =
      "{\"profile\":\"nope\",\"layout\":\"gallery\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"\","
      "\"dial_code\":\"\"}";
  static const char bad_layout[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"grid\","
      "\"meeting_id\":\"1\",\"passcode\":\"\",\"host_key\":\"\","
      "\"dial_code\":\"\"}";
  static const char numeric_meeting[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":1,\"passcode\":\"\",\"host_key\":\"\","
      "\"dial_code\":\"\"}";
  static const char duplicate_key[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":\"1\",\"meeting_id\":\"2\",\"host_key\":\"\","
      "\"dial_code\":\"\"}";
  static const char escaped_value[] =
      "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
      "\"meeting_id\":\"1\\n\",\"passcode\":\"\",\"host_key\":\"\","
      "\"dial_code\":\"\"}";
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(missing_member),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(unknown_member),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(bad_profile),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(bad_layout),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(numeric_meeting),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(duplicate_key),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(escaped_value),
                    &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes("{}"), &request),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(""), &request),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes("{}"), NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
  /* A rejected payload leaves nothing bound in the output. */
  AULA_CHECK(request.dial.meeting_id == NULL);
  AULA_CHECK(request.meeting_id[0] == '\0');
}

static void test_originate_field_bounds(void) {
  /* Meeting IDs hold up to 16 characters, passcodes 32, host keys 16 and
   * dial codes 64; one more is refused. */
  char payload[512];
  static const size_t limits[] = {AULA_ZOOM_MAX_MEETING_ID_BYTES - 1U,
                                  AULA_ZOOM_MAX_PASSCODE_BYTES - 1U,
                                  AULA_ZOOM_MAX_HOST_KEY_BYTES - 1U,
                                  AULA_ZOOM_MAX_DIAL_CODE_BYTES - 1U};
  size_t field;
  for (field = 0U; field < 4U; ++field) {
    size_t over;
    for (over = 0U; over < 2U; ++over) {
      char value[96];
      char members[4][100] = {"\"1\"", "\"\"", "\"\"", "\"\""};
      aula_control_originate_request request;
      size_t length = limits[field] + over;
      int written;
      (void)memset(value + 1, '7', length);
      value[0] = '"';
      value[length + 1U] = '"';
      value[length + 2U] = '\0';
      (void)memcpy(members[field], value, length + 3U);
      written = snprintf(payload, sizeof(payload),
                         "{\"profile\":\"zoom_direct\",\"layout\":\"gallery\","
                         "\"meeting_id\":%s,\"passcode\":%s,\"host_key\":%s,"
                         "\"dial_code\":%s}",
                         members[0], members[1], members[2], members[3]);
      AULA_CHECK(written > 0 && (size_t)written < sizeof(payload));
      AULA_CHECK_STATUS(aula_control_decode_originate(text_bytes(payload),
                        &request),
                        over == 0U ? AULA_STATUS_OK : AULA_STATUS_INVALID_DATA);
    }
  }
}

static void test_dtmf(void) {
  static const char symbols[] = "0123456789*#ABCD";
  uint8_t digit = 0xffU;
  size_t index;
  for (index = 0U; index < sizeof(symbols) - 1U; ++index) {
    char payload[32];
    (void)snprintf(payload, sizeof(payload), "{\"tone\":\"%c\"}", symbols[index]);
    digit = 0xffU;
    AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes(payload), &digit),
                      AULA_STATUS_OK);
    AULA_CHECK(digit == (uint8_t)index);
  }
  digit = 0xffU;
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"E\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"a\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"12\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":5}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"digit\":\"5\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(
      text_bytes("{\"tone\":\"5\",\"extra\":\"x\"}"), &digit),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{}"), &digit),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"5\"}"),
                    NULL), AULA_STATUS_INVALID_ARGUMENT);
  AULA_CHECK(digit == 0xffU);
}

static void test_credentials(void) {
  aula_control_credentials_request request;
  char payload[1024];
  char username[260];
  char password[260];
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"username\":\"room-7\",\"password\":\"correct-horse-battery\"}"),
      &request), AULA_STATUS_OK);
  AULA_CHECK(strcmp(request.username, "room-7") == 0);
  AULA_CHECK(strcmp(request.password, "correct-horse-battery") == 0);
  /* Member order does not matter; passwords need at least 12 characters. */
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"password\":\"123456789012\",\"username\":\"u\"}"), &request),
      AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"password\":\"12345678901\",\"username\":\"u\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK(request.password[0] == '\0' && request.username[0] == '\0');
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"password\":\"123456789012\",\"username\":\"\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"username\":\"u\"}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"username\":\"u\",\"password\":123456789012}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(
      "{\"username\":\"u\",\"password\":\"123456789012\",\"x\":\"y\"}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes("{}"), NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
  /* The JSON string reader caps a value at 256 characters, so the 257-byte
   * output buffers are never reached with a longer value. */
  (void)memset(username, 'u', 256U);
  username[256] = '\0';
  (void)memset(password, 'p', 256U);
  password[256] = '\0';
  (void)snprintf(payload, sizeof(payload),
                 "{\"username\":\"%s\",\"password\":\"%s\"}", username, password);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(payload),
                    &request), AULA_STATUS_OK);
  AULA_CHECK(strlen(request.username) == 256U);
  AULA_CHECK(strlen(request.password) == 256U);
  username[256] = 'u';
  username[257] = '\0';
  (void)snprintf(payload, sizeof(payload),
                 "{\"username\":\"%s\",\"password\":\"%s\"}", username, password);
  AULA_CHECK_STATUS(aula_control_decode_credentials(text_bytes(payload),
                    &request), AULA_STATUS_INVALID_DATA);
}

static void test_media(void) {
  aula_control_media_request request;
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"mode\":\"enabled\"}"), &request), AULA_STATUS_OK);
  AULA_CHECK(request.action == AULA_CONTROL_MEDIA_VIDEO_TRANSMIT);
  AULA_CHECK(request.video_transmit_enabled == 1);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"mode\":\"disabled\"}"), &request), AULA_STATUS_OK);
  AULA_CHECK(request.action == AULA_CONTROL_MEDIA_VIDEO_TRANSMIT);
  AULA_CHECK(request.video_transmit_enabled == 0);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"audio_mute\",\"value\":\"enabled\"}"),
      &request), AULA_STATUS_OK);
  AULA_CHECK(request.action == AULA_CONTROL_MEDIA_AUDIO_MUTE);
  AULA_CHECK(request.audio_muted == 1);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"audio_mute\",\"value\":\"disabled\"}"),
      &request), AULA_STATUS_OK);
  AULA_CHECK(request.audio_muted == 0);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"keyframe\",\"value\":\"request\"}"), &request),
      AULA_STATUS_OK);
  AULA_CHECK(request.action == AULA_CONTROL_MEDIA_KEYFRAME);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"layout_next\",\"value\":\"request\"}"),
      &request), AULA_STATUS_OK);
  AULA_CHECK(request.action == AULA_CONTROL_MEDIA_LAYOUT_NEXT);
}

static void test_media_rejects_bad_payloads(void) {
  aula_control_media_request request;
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"mode\":\"on\"}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"mode\":true}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"mode\":\"enabled\",\"action\":\"keyframe\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"audio_mute\",\"value\":\"request\"}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"keyframe\",\"value\":\"enabled\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"reboot\",\"value\":\"request\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      text_bytes("{\"action\":\"keyframe\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(text_bytes("{}"), &request),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(text_bytes(""), &request),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(text_bytes("{}"), NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
}

/* Pads a payload with trailing spaces, which the parser skips, to an exact
 * length so that only the size cap decides the outcome. */
static aula_bytes padded_payload(char *buffer, size_t buffer_bytes,
                                 const char *json, size_t length) {
  size_t used = strlen(json);
  aula_bytes bytes = {NULL, 0U};
  if (used > length || length >= buffer_bytes) return bytes;
  (void)memcpy(buffer, json, used);
  (void)memset(buffer + used, ' ', length - used);
  buffer[length] = '\0';
  bytes.data = (const uint8_t *)buffer;
  bytes.length = length;
  return bytes;
}

static void test_media_size_bounds(void) {
  aula_control_media_request request;
  char buffer[128];
  /* Video-mode payloads are capped at 32 bytes, action payloads at 48. */
  static const char video[] = "{\"mode\":\"enabled\"}";
  static const char action[] = "{\"action\":\"keyframe\",\"value\":\"request\"}";
  AULA_CHECK_STATUS(aula_control_decode_media(
      padded_payload(buffer, sizeof(buffer), video, 32U), &request),
      AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_control_decode_media(
      padded_payload(buffer, sizeof(buffer), video, 33U), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_media(
      padded_payload(buffer, sizeof(buffer), action, 48U), &request),
      AULA_STATUS_OK);
  AULA_CHECK_STATUS(aula_control_decode_media(
      padded_payload(buffer, sizeof(buffer), action, 49U), &request),
      AULA_STATUS_INVALID_DATA);
}

static void test_settings(void) {
  aula_control_settings_request request;
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":7}"),
      &request), AULA_STATUS_OK);
  AULA_CHECK(request.revision == 7U);
  AULA_CHECK(request.profile == AULA_ZOOM_PROFILE_DIRECT_CRC);
  AULA_CHECK(request.media_managed == 1);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"private_lab\",\"media\":\"disabled\","
      "\"revision\":4294967295,\"tls\":\"verified\"}"), &request),
      AULA_STATUS_OK);
  AULA_CHECK(request.revision == UINT32_MAX);
  AULA_CHECK(request.profile == AULA_ZOOM_PROFILE_PRIVATE_LAB);
  AULA_CHECK(request.media_managed == 0);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"tls\":\"required\",\"revision\":1,\"media\":\"managed\","
      "\"profile\":\"zoom_proxy\"}"), &request), AULA_STATUS_OK);
  AULA_CHECK(request.profile == AULA_ZOOM_PROFILE_PROXY_REGISTRATION);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":1,"
      "\"tls\":\"not_required\"}"), &request), AULA_STATUS_OK);
}

static void test_json_strictness(void) {
  aula_control_settings_request request;
  /* JSON numbers have no leading zeros. */
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":07}"),
      &request), AULA_STATUS_INVALID_DATA);
  /* Only JSON whitespace separates tokens; a vertical tab or form feed is
   * not accepted, while tab, CR and LF are. */
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\v\"media\":\"managed\",\"revision\":7}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\f\"media\":\"managed\",\"revision\":7}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\t\"profile\":\"zoom_direct\",\r\n\"media\":\"managed\",\"revision\":7 }"),
      &request), AULA_STATUS_OK);
}

static void test_settings_rejects_bad_payloads(void) {
  aula_control_settings_request request;
  /* Revision zero, overflow, quoting and non-digits. */
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":0}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\","
      "\"revision\":4294967296}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\","
      "\"revision\":\"7\"}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\","
      "\"revision\":-1}"), &request), AULA_STATUS_INVALID_DATA);
  /* Missing and unknown members. */
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\"}"), &request),
      AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"revision\":1,\"extra\":\"x\"}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":1,"
      "\"extra\":\"x\"}"), &request), AULA_STATUS_INVALID_DATA);
  /* Unknown enum tokens. */
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"other\",\"media\":\"managed\",\"revision\":1}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"auto\",\"revision\":1}"),
      &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes(
      "{\"profile\":\"zoom_direct\",\"media\":\"managed\",\"revision\":1,"
      "\"tls\":\"off\"}"), &request), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes("{}"), &request),
                    AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_settings(text_bytes("{}"), NULL),
                    AULA_STATUS_INVALID_ARGUMENT);
}

static void test_json_subset_limits(void) {
  uint8_t digit = 0U;
  /* Trailing garbage, a missing brace, a missing colon, a trailing comma and
   * raw control or high bytes are all malformed. */
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"5\"}x"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"5\""),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\" \"5\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"5\",}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"\x01\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes("{\"tone\":\"\xc3\xa9\"}"),
                    &digit), AULA_STATUS_INVALID_DATA);
  /* Whitespace between tokens is tolerated. */
  AULA_CHECK_STATUS(aula_control_decode_dtmf(
      text_bytes(" {\n\t\"tone\" : \"5\" } \n"), &digit), AULA_STATUS_OK);
  AULA_CHECK(digit == 5U);
  /* An object is limited to six members. */
  AULA_CHECK_STATUS(aula_control_decode_dtmf(text_bytes(
      "{\"a\":\"1\",\"b\":\"1\",\"c\":\"1\",\"d\":\"1\",\"e\":\"1\","
      "\"f\":\"1\",\"g\":\"1\"}"), &digit), AULA_STATUS_INVALID_DATA);
  /* A NULL payload with a non-zero length is not read. */
  AULA_CHECK_STATUS(aula_control_decode_dtmf((aula_bytes){NULL, 4U}, &digit),
                    AULA_STATUS_INVALID_DATA);
}

int main(void) {
  test_empty_object();
  test_originate_accepts_full_object();
  test_originate_profiles_and_layouts();
  test_originate_rejects_bad_payloads();
  test_originate_field_bounds();
  test_dtmf();
  test_credentials();
  test_media();
  test_media_rejects_bad_payloads();
  test_media_size_bounds();
  test_settings();
  test_settings_rejects_bad_payloads();
  test_json_strictness();
  test_json_subset_limits();
  return aula_test_result("test_request_decode");
}
