/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Owned loopback-only SIP fixture for the original-kernel rescue guest.
 * This tests real daemon signaling; it supplies inactive media, not a media pass.
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PACKET_BYTES = 8192, STATE_IDLE = 1, STATE_ESTABLISHED = 2,
       STATE_TERMINATED = 3, STATE_OTHER = 4 };
static volatile sig_atomic_t interrupted;
static uint32_t next_request = 1U;

struct peer {
  int descriptor;
  unsigned int port;
  unsigned int invites, acks, byes;
  struct sockaddr_in daemon;
  char from[512], to[512], call_id[256];
  unsigned long invite_cseq;
};

static void handle_signal(int number) { (void)number; interrupted = 1; }
static uint64_t milliseconds(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0) return UINT64_MAX;
  return (uint64_t)value.tv_sec * UINT64_C(1000) + (uint64_t)value.tv_nsec / UINT64_C(1000000);
}
static int wait_exit(pid_t child, unsigned int timeout, int *status) {
  uint64_t deadline = milliseconds() + timeout;
  for (;;) {
    pid_t result = waitpid(child, status, WNOHANG);
    if (result == child) return 0;
    if (result < 0 && errno != EINTR) return -1;
    if (milliseconds() >= deadline) return -1;
    (void)poll(NULL, 0U, 10);
  }
}
static int write_file(const char *path, const char *value) {
  int descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
  size_t length = strlen(value), offset = 0U;
  if (descriptor < 0) return -1;
  while (offset < length) {
    ssize_t count = write(descriptor, value + offset, length - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { (void)close(descriptor); return -1; }
    offset += (size_t)count;
  }
  return close(descriptor);
}
static void store_u32(unsigned char *data, uint32_t value) {
  value = htonl(value); (void)memcpy(data, &value, sizeof(value));
}
static uint32_t load_u32(const unsigned char *data) {
  uint32_t value; (void)memcpy(&value, data, sizeof(value)); return ntohl(value);
}

static int status_state(const char *payload) {
  const char *cursor = strstr(payload, "\"call_state\"");
  if (cursor == NULL) return -1;
  cursor += strlen("\"call_state\"");
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
  if (*cursor++ != ':') return -1;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
  if (strncmp(cursor, "\"idle\"", 6U) == 0) return STATE_IDLE;
  if (strncmp(cursor, "\"established\"", 13U) == 0) return STATE_ESTABLISHED;
  if (strncmp(cursor, "\"terminated\"", 12U) == 0) return STATE_TERMINATED;
  return STATE_OTHER;
}
static const char *skip_json_space(const char *cursor) {
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
  return cursor;
}
static int mutation_succeeded(const char *payload) {
  const char *cursor = skip_json_space(payload);
  if (*cursor++ != '{') return 0;
  cursor = skip_json_space(cursor);
  if (strncmp(cursor, "\"ok\"", 4U) != 0) return 0;
  cursor = skip_json_space(cursor + 4U);
  if (*cursor++ != ':') return 0;
  cursor = skip_json_space(cursor);
  if (strncmp(cursor, "true", 4U) != 0) return 0;
  cursor = skip_json_space(cursor + 4U);
  if (*cursor++ != '}') return 0;
  return *skip_json_space(cursor) == '\0';
}
static int decode_control_reply(unsigned char packet[PACKET_BYTES], size_t count,
                                unsigned char opcode, uint32_t request) {
  if (count < 16U || count >= PACKET_BYTES || memcmp(packet, "LSZ1", 4U) != 0 ||
      packet[4] != 1U || packet[5] != opcode || packet[6] != 0U || packet[7] != 1U ||
      load_u32(packet + 8U) != request || load_u32(packet + 12U) != count - 16U ||
      memchr(packet + 16U, '\0', count - 16U) != NULL) return -1;
  packet[count] = '\0';
  return opcode == 1U ? status_state((const char *)packet + 16U) :
      mutation_succeeded((const char *)packet + 16U) ? 0 : -1;
}
static int decode_request_line(const char *packet, char method[16]) {
  const char *end = strstr(packet, "\r\n");
  char line[1024], uri[512], version[16], extra;
  if (end == NULL || (size_t)(end - packet) >= sizeof(line)) return -1;
  (void)memcpy(line, packet, (size_t)(end - packet)); line[end - packet] = '\0';
  return sscanf(line, "%15s %511s %15s %c", method, uri, version, &extra) == 3 &&
      strcmp(version, "SIP/2.0") == 0 && strncmp(uri, "sip:", 4U) == 0 && uri[4] != '\0' ? 0 : -1;
}
static int self_test(void) {
  static const char *const invalid[] = {"{\"ok\":false}", "{\"ok\":1}", "not-json", "{\"ok\":true}extra", "{\"ok\":true,\"extra\":1}"};
  unsigned char packet[PACKET_BYTES];
  char method[16];
  size_t index;
  if (!mutation_succeeded(" \r\n{ \"ok\" : true }\t") ||
      decode_request_line("INVITE sip:room@127.0.0.1 SIP/2.0\r\n", method) != 0 || strcmp(method, "INVITE") != 0 ||
      decode_request_line("INVITE sip:room@127.0.0.1 SIP/1.0\r\n", method) == 0 ||
      decode_request_line("INVITE sip:room@127.0.0.1 SIP/2.0 extra\r\n", method) == 0 ||
      decode_request_line("INVITE http://127.0.0.1 SIP/2.0\r\n", method) == 0) return 1;
  (void)memset(packet, 0, sizeof(packet)); (void)memcpy(packet, "LSZ1", 4U);
  packet[4] = 1U; packet[5] = 2U; packet[7] = 1U; store_u32(packet + 8U, 7U);
  (void)memcpy(packet + 16U, "{\"ok\":true}", 11U); store_u32(packet + 12U, 11U);
  if (decode_control_reply(packet, 27U, 2U, 7U) != 0 || decode_control_reply(packet, 27U, 2U, 8U) >= 0) return 1;
  packet[7] = 3U; if (decode_control_reply(packet, 27U, 2U, 7U) >= 0) return 1; packet[7] = 1U;
  for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
    size_t length = strlen(invalid[index]);
    (void)memcpy(packet + 16U, invalid[index], length); store_u32(packet + 12U, (uint32_t)length);
    if (decode_control_reply(packet, length + 16U, 2U, 7U) >= 0) return 1;
  }
  (void)memcpy(packet + 16U, "{\"ok\":true}\0junk", 16U); store_u32(packet + 12U, 16U);
  if (decode_control_reply(packet, 32U, 2U, 7U) >= 0) return 1;
  (void)puts("AULA_GUEST_SIP_SELF_TEST_PASS"); return 0;
}

/* Run the production peer check as a distinct gateway UID. Only its bounded
 * result crosses a pipe; no authenticated daemon behavior is bypassed. */
static int gateway_request(const char *path, unsigned char opcode,
                           uint32_t request, const char *payload) {
  struct sockaddr_un address;
  unsigned char packet[PACKET_BYTES];
  struct timeval timeout = {2, 0};
  size_t length = strlen(payload);
  ssize_t count;
  int descriptor, result = -1;
  if (length + 16U >= sizeof(packet) || strlen(path) >= sizeof(address.sun_path)) return -1;
  if (setgroups(0U, NULL) != 0 || setgid(0U) != 0 || setuid(10001U) != 0 || geteuid() != 10001U) return -1;
  descriptor = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (descriptor < 0) return -1;
  (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  (void)memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  (void)memcpy(address.sun_path, path, strlen(path) + 1U);
  if (connect(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0) goto done;
  (void)memcpy(packet, "LSZ1", 4U); packet[4] = 1U; packet[5] = opcode;
  packet[6] = 0U; packet[7] = 0U; store_u32(packet + 8U, request);
  store_u32(packet + 12U, (uint32_t)length); (void)memcpy(packet + 16U, payload, length);
  count = send(descriptor, packet, length + 16U, MSG_NOSIGNAL);
  if (count != (ssize_t)(length + 16U)) goto done;
  count = recv(descriptor, packet, sizeof(packet) - 1U, 0);
  if (count < 0) goto done;
  result = decode_control_reply(packet, (size_t)count, opcode, request);
done:
  (void)close(descriptor); return result;
}
static int control(const char *path, unsigned char opcode, const char *payload) {
  int descriptors[2], result = -1, status = 0;
  struct pollfd ready;
  pid_t child;
  uint32_t request = next_request++;
  if (pipe(descriptors) != 0) return -1;
  child = fork();
  if (child == 0) {
    int reply;
    (void)close(descriptors[0]);
    reply = gateway_request(path, opcode, request, payload);
    if (write(descriptors[1], &reply, sizeof(reply)) != (ssize_t)sizeof(reply)) _exit(2);
    (void)close(descriptors[1]); _exit(reply < 0 ? 1 : 0);
  }
  (void)close(descriptors[1]);
  if (child < 0) { (void)close(descriptors[0]); return -1; }
  ready.fd = descriptors[0]; ready.events = POLLIN; ready.revents = 0;
  if (poll(&ready, 1U, 3000) > 0 && read(descriptors[0], &result, sizeof(result)) == (ssize_t)sizeof(result)) {
    if (wait_exit(child, 1000U, &status) != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) result = -1;
  } else { result = -1; }
  (void)close(descriptors[0]);
  if (waitpid(child, &status, WNOHANG) == 0) { (void)kill(child, SIGKILL); (void)wait_exit(child, 1000U, &status); }
  return result;
}

static int header(const char *packet, const char *name, char *output, size_t capacity) {
  const char *cursor = strstr(packet, "\r\n");
  size_t name_length = strlen(name);
  int matches = 0;
  if (cursor == NULL) return -1;
  cursor += 2;
  while (*cursor != '\0' && strncmp(cursor, "\r\n", 2U) != 0) {
    const char *end = strstr(cursor, "\r\n");
    if (end == NULL) return -1;
    if ((size_t)(end - cursor) > name_length && strncasecmp(cursor, name, name_length) == 0 && cursor[name_length] == ':') {
      const char *value = cursor + name_length + 1U;
      size_t length;
      while (value < end && (*value == ' ' || *value == '\t')) ++value;
      length = (size_t)(end - value);
      if (++matches != 1 || length >= capacity) return -1;
      (void)memcpy(output, value, length); output[length] = '\0';
    }
    cursor = end + 2;
  }
  return matches == 1 ? 0 : -1;
}
static int respond(struct peer *peer, const struct sockaddr_in *target,
                   const char *via, const char *cseq, int code, const char *reason, const char *body) {
  char response[PACKET_BYTES];
  int length = snprintf(response, sizeof(response),
      "SIP/2.0 %d %s\r\nVia: %s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
      "Contact: <sip:peer@127.0.0.1:%u;transport=udp>\r\n%sContent-Length: %lu\r\n\r\n%s",
      code, reason, via, peer->from, peer->to, peer->call_id, cseq, peer->port,
      body[0] != '\0' ? "Content-Type: application/sdp\r\n" : "", (unsigned long)strlen(body), body);
  if (length < 0 || (size_t)length >= sizeof(response)) return -1;
  return sendto(peer->descriptor, response, (size_t)length, 0,
                (const struct sockaddr *)target, sizeof(*target)) == length ? 0 : -1;
}
static int hex_value(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}
static int make_sdp(const char *offer, char *answer, size_t capacity) {
  static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const char *profile = strstr(offer, "profile-level-id=");
  unsigned char sps[4] = {0x67U, 0U, 0U, 0U};
  char identifier[7], encoded[9];
  unsigned int index;
  int length;
  if (profile == NULL || strlen(profile + 17U) < 6U) return -1;
  profile += 17U;
  for (index = 0U; index < 6U; ++index) if (hex_value((unsigned char)profile[index]) < 0) return -1;
  (void)memcpy(identifier, profile, 6U); identifier[6] = '\0';
  for (index = 0U; index < 3U; ++index)
    sps[index + 1U] = (unsigned char)(hex_value((unsigned char)profile[index * 2U]) * 16 + hex_value((unsigned char)profile[index * 2U + 1U]));
  encoded[0] = base64[sps[0] >> 2U]; encoded[1] = base64[((sps[0] & 3U) << 4U) | (sps[1] >> 4U)];
  encoded[2] = base64[((sps[1] & 15U) << 2U) | (sps[2] >> 6U)]; encoded[3] = base64[sps[2] & 63U];
  encoded[4] = base64[sps[3] >> 2U]; encoded[5] = base64[(sps[3] & 3U) << 4U];
  encoded[6] = '='; encoded[7] = '='; encoded[8] = '\0';
  length = snprintf(answer, capacity,
      "v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=aula-owned-guest-peer\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
      "m=video 45000 RTP/AVP 96\r\na=rtcp-mux\r\na=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,aM4G4g==\r\na=inactive\r\n"
      "m=audio 45002 RTP/AVP 0 8 101\r\na=rtcp-mux\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n"
      "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-16\r\na=inactive\r\n", identifier, encoded);
  return length > 0 && (size_t)length < capacity ? 0 : -1;
}
static int service_peer(struct peer *peer) {
  char packet[PACKET_BYTES], via[512], from[512], to[512], call_id[256], cseq[128], length_text[32];
  char method[16], cseq_method[16], extra;
  char *body, *end;
  unsigned long sequence, declared;
  struct sockaddr_in source;
  socklen_t source_length = sizeof(source);
  ssize_t count = recvfrom(peer->descriptor, packet, sizeof(packet) - 1U, 0, (struct sockaddr *)&source, &source_length);
  if (count <= 0 || source_length != sizeof(source) || source.sin_family != AF_INET ||
      source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return -1;
  packet[(size_t)count] = '\0';
  if (strlen(packet) != (size_t)count || decode_request_line(packet, method) != 0 ||
      header(packet, "Via", via, sizeof(via)) != 0 || header(packet, "From", from, sizeof(from)) != 0 ||
      header(packet, "To", to, sizeof(to)) != 0 || header(packet, "Call-ID", call_id, sizeof(call_id)) != 0 ||
      header(packet, "CSeq", cseq, sizeof(cseq)) != 0 || header(packet, "Content-Length", length_text, sizeof(length_text)) != 0 ||
      sscanf(cseq, "%lu %15s %c", &sequence, cseq_method, &extra) != 2 || strcmp(method, cseq_method) != 0) return -1;
  body = strstr(packet, "\r\n\r\n"); if (body == NULL) return -1; body += 4;
  errno = 0; declared = strtoul(length_text, &end, 10);
  if (errno != 0 || end == length_text || *end != '\0' || declared != strlen(body)) return -1;
  if (strcmp(method, "INVITE") == 0) {
    char answer[2048];
    if (++peer->invites != 1U || peer->acks != 0U || peer->byes != 0U || sequence == 0U ||
        strstr(to, ";tag=") != NULL || make_sdp(body, answer, sizeof(answer)) != 0) return -1;
    peer->daemon = source; peer->invite_cseq = sequence;
    (void)snprintf(peer->from, sizeof(peer->from), "%s", from);
    (void)snprintf(peer->call_id, sizeof(peer->call_id), "%s", call_id);
    if (snprintf(peer->to, sizeof(peer->to), "%s;tag=aula-owned-guest", to) >= (int)sizeof(peer->to)) return -1;
    if (respond(peer, &source, via, cseq, 100, "Trying", "") != 0 ||
        respond(peer, &source, via, cseq, 180, "Ringing", "") != 0 ||
        respond(peer, &source, via, cseq, 200, "OK", answer) != 0) return -1;
    return 0;
  }
  if (source.sin_port != peer->daemon.sin_port || strcmp(call_id, peer->call_id) != 0 ||
      strcmp(from, peer->from) != 0 || strcmp(to, peer->to) != 0 || declared != 0U) return -1;
  if (strcmp(method, "ACK") == 0)
    return ++peer->acks == 1U && peer->invites == 1U && peer->byes == 0U && sequence == peer->invite_cseq ? 0 : -1;
  if (strcmp(method, "BYE") == 0) {
    if (++peer->byes != 1U || peer->acks != 1U || sequence <= peer->invite_cseq) return -1;
    return respond(peer, &source, via, cseq, 200, "OK", "");
  }
  return -1;
}

static pid_t launch_daemon(const char *daemon, const char *qemu, const char *runtime,
                           const char *config, const char *log) {
  pid_t child = fork();
  if (child == 0) {
    char *const environment[] = {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", NULL};
    int descriptor = open(log, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0 || dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0) _exit(125);
    (void)close(descriptor);
    if (qemu != NULL) {
      char *const arguments[] = {(char *)qemu, "-cpu", "cortex-a8", "-L", (char *)runtime,
                                (char *)daemon, "--foreground", "--config", (char *)config, NULL};
      execve(qemu, arguments, environment);
    } else {
      char *const arguments[] = {(char *)daemon, "--foreground", "--config", (char *)config, NULL};
      execve(daemon, arguments, environment);
    }
    _exit(126);
  }
  return child;
}

int main(int argc, char **argv) {
  const char *daemon = "/opt/aula/aula-sipd", *qemu = NULL, *runtime = NULL, *stage = "arguments";
  char directory[] = "/tmp/aula-sip-smoke-XXXXXX";
  char control_path[128], config_path[128], video_path[128], audio_path[128], log_path[128], pid_path[128], settings_path[128];
  char config[4096];
  struct sockaddr_in address;
  socklen_t address_length = sizeof(address);
  struct peer peer;
  struct stat details;
  pid_t child = -1;
  int status = 0, result = 1, index, have_directory = 0;
  uint64_t deadline;
  (void)memset(&peer, 0, sizeof(peer)); peer.descriptor = -1;
  if (argc == 2 && strcmp(argv[1], "--self-test") == 0) return self_test();
  for (index = 1; index < argc; ++index) {
    if (index + 1 >= argc) goto done;
    if (strcmp(argv[index], "--daemon") == 0) daemon = argv[++index];
    else if (strcmp(argv[index], "--qemu-arm") == 0) qemu = argv[++index];
    else if (strcmp(argv[index], "--runtime-root") == 0) runtime = argv[++index];
    else goto done;
  }
  if (geteuid() != 0U || daemon[0] != '/' || (qemu == NULL) != (runtime == NULL) ||
      (qemu != NULL && (qemu[0] != '/' || runtime[0] != '/'))) goto done;
  stage = "temporary-directory";
  if (mkdtemp(directory) == NULL || chmod(directory, 0755) != 0) goto done;
  have_directory = 1;
#define FIXTURE_PATH(destination, name) (void)snprintf(destination, sizeof(destination), "%s/" name, directory)
  FIXTURE_PATH(control_path, "control.sock"); FIXTURE_PATH(config_path, "daemon.conf");
  FIXTURE_PATH(video_path, "video.h264"); FIXTURE_PATH(audio_path, "audio.pcm");
  FIXTURE_PATH(log_path, "daemon.log"); FIXTURE_PATH(pid_path, "daemon.pid"); FIXTURE_PATH(settings_path, "settings.json");
#undef FIXTURE_PATH
  stage = "loopback-peer";
  peer.descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  (void)memset(&address, 0, sizeof(address)); address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (peer.descriptor < 0 || bind(peer.descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
      getsockname(peer.descriptor, (struct sockaddr *)&address, &address_length) != 0) goto done;
  peer.port = ntohs(address.sin_port);
  stage = "configuration";
  if (snprintf(config, sizeof(config),
      "[runtime]\nforeground = true\npid_file = %s\nsettings_file = %s\nlog_sink = syslog\n"
      "[sip]\nprofile = private_lab\nuri = sip:aula@127.0.0.1:%u\ntransport = udp\nenable_tls = false\nenable_public_network = true\n"
      "[media]\nbackend = fixture\nvideo_source = %s\naudio_source = %s\nbind_address = 127.0.0.1\nadvertised_address = 127.0.0.1\nsecurity = plain_compat\n"
      "[control]\nenable_local_control = true\nunix_socket_path = %s\nunix_socket_mode = 0660\ngateway_uid = 10001\n",
      pid_path, settings_path, peer.port, video_path, audio_path, control_path) >= (int)sizeof(config) ||
      write_file(video_path, "") != 0 || write_file(audio_path, "") != 0 || write_file(config_path, config) != 0) goto done;
  stage = "daemon-startup";
  (void)signal(SIGINT, handle_signal); (void)signal(SIGTERM, handle_signal);
  child = launch_daemon(daemon, qemu, runtime, config_path, log_path);
  if (child < 0) goto done;
  deadline = milliseconds() + 10000U;
  for (;;) {
    if (lstat(control_path, &details) == 0) { if (!S_ISSOCK(details.st_mode)) goto done; break; }
    if (waitpid(child, &status, WNOHANG) == child) { child = -1; goto done; }
    if (interrupted || milliseconds() >= deadline) goto done;
    (void)poll(NULL, 0U, 20);
  }
  stage = "initial-idle";
  if (control(control_path, 1U, "{}") != STATE_IDLE) goto done;
  stage = "originate";
  if (control(control_path, 2U, "{\"profile\":\"private_lab\",\"layout\":\"gallery\",\"meeting_id\":\"123456789\",\"passcode\":\"\",\"host_key\":\"\",\"dial_code\":\"\"}") != 0) goto done;
  stage = "establish"; deadline = milliseconds() + 15000U;
  for (;;) {
    struct pollfd ready = {peer.descriptor, POLLIN, 0};
    int available = poll(&ready, 1U, 20);
    if (available < 0 && errno != EINTR) goto done;
    if (available > 0 && service_peer(&peer) != 0) goto done;
    if (control(control_path, 1U, "{}") == STATE_ESTABLISHED && peer.acks == 1U) break;
    if (interrupted || milliseconds() >= deadline) goto done;
  }
  stage = "hangup";
  if (control(control_path, 3U, "{}") != 0) goto done;
  deadline = milliseconds() + 15000U;
  for (;;) {
    struct pollfd ready = {peer.descriptor, POLLIN, 0};
    int available = poll(&ready, 1U, 20);
    if (available < 0 && errno != EINTR) goto done;
    if (available > 0 && service_peer(&peer) != 0) goto done;
    if (control(control_path, 1U, "{}") == STATE_TERMINATED && peer.byes == 1U) break;
    if (interrupted || milliseconds() >= deadline) goto done;
  }
  stage = "exact-dialog";
  if (peer.invites != 1U || peer.acks != 1U || peer.byes != 1U) goto done;
  {
    struct pollfd ready = {peer.descriptor, POLLIN, 0};
    if (poll(&ready, 1U, 100) != 0) goto done;
  }
  stage = "clean-daemon-exit";
  if (kill(child, SIGTERM) != 0 || wait_exit(child, 5000U, &status) != 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) goto done;
  child = -1; result = 0;
done:
  if (child > 0) { (void)kill(child, SIGTERM); if (wait_exit(child, 1000U, &status) != 0) { (void)kill(child, SIGKILL); (void)wait_exit(child, 1000U, &status); } }
  if (peer.descriptor >= 0) (void)close(peer.descriptor);
  if (result == 0) {
    const char *files[] = {control_path, config_path, video_path, audio_path, log_path, pid_path, settings_path};
    size_t item;
    for (item = 0U; item < sizeof(files) / sizeof(files[0]); ++item) if (unlink(files[item]) != 0 && errno != ENOENT) result = 1;
    if (rmdir(directory) != 0) result = 1;
    if (result == 0) (void)puts("{\"marker\":\"AULA_GUEST_SIP_PASS\",\"invite\":1,\"ack\":1,\"bye\":1,\"initial\":\"idle\",\"established\":\"established\",\"final\":\"terminated\",\"daemon_exit\":0,\"media_io\":false}");
  }
  if (result != 0) (void)fprintf(stderr, "AULA_GUEST_SIP_FAIL stage=%s errno=%d invite=%u ack=%u bye=%u diagnostic_directory=%s\n", stage, errno, peer.invites, peer.acks, peer.byes, have_directory ? directory : "none");
  return result;
}
