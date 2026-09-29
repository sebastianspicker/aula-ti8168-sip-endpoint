#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#if defined(__linux__)
#define _GNU_SOURCE 1
#endif

#include "control_private.h"
#include "request_decode.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void protocol_secure_zero(void *memory, size_t length) {
  volatile unsigned char *cursor = (volatile unsigned char *)memory;
  while (length > 0U) {
    *cursor++ = 0U;
    --length;
  }
}

static const char *call_state_name(aula_call_state state) {
  static const char *const names[] = {
    "idle", "resolving", "inviting", "early", "establishing_media",
    "established", "terminating", "backing_off", "failed", "terminated",
    "stopped", "terminal_failure"};
  return state >= AULA_CALL_IDLE && state <= AULA_CALL_TERMINAL_FAILURE
      ? names[state] : "invalid";
}

static const char *media_state_name(aula_media_session_state state) {
  static const char *const names[] = {
    "new", "reserved", "preparing", "prepared", "committed", "stopped"};
  return state >= AULA_MEDIA_SESSION_NEW && state <= AULA_MEDIA_SESSION_STOPPED
      ? names[state] : "invalid";
}

static const char *direction_name(aula_sdp_direction direction) {
  static const char *const names[] = {"sendrecv", "sendonly", "recvonly", "inactive"};
  return direction >= AULA_SDP_SENDRECV && direction <= AULA_SDP_INACTIVE
      ? names[direction] : "invalid";
}

static int profile_is_safe(const char profile[7]);

static const char *json_bool(int value) {
  return value != 0 ? "true" : "false";
}

static const char *status_profile(const aula_endpoint_status *status) {
  return profile_is_safe(status->active_h264_profile_level_id)
      ? status->active_h264_profile_level_id : "none";
}

static const char *status_media_state(const aula_endpoint_status *status) {
  return status->media_session_present
      ? media_state_name(status->media_session_state) : "none";
}

static const char *status_sip_profile(const aula_endpoint_status *status) {
  static const char *const names[] = {"zoom_direct", "zoom_proxy", "private_lab"};
  return status->sip_profile <= AULA_ZOOM_PROFILE_PRIVATE_LAB
      ? names[status->sip_profile] : "invalid";
}

static const char *status_sip_transport(const aula_endpoint_status *status) {
  static const char *const names[] = {"udp", "tcp", "tls"};
  return status->sip_transport <= AULA_TRANSPORT_TLS
      ? names[status->sip_transport] : "invalid";
}

static const char *status_registration(const aula_endpoint_status *status) {
  static const char *const names[] = {"disabled", "registering", "registered", "failed"};
  return status->registration.state <= AULA_SIP_REGISTRATION_FAILED
      ? names[status->registration.state] : "failed";
}

static const char *status_aec_state(const aula_endpoint_status *status) {
  static const char *const names[] = {"inactive", "available", "uncalibrated", "calibrated"};
  return status->aec.delay_state <= AULA_AEC_DELAY_CALIBRATED
      ? names[status->aec.delay_state] : "inactive";
}

static int profile_is_safe(const char profile[7]) {
  size_t index;
  if (profile == NULL || profile[6] != '\0') return 0;
  for (index = 0U; index < 6U; ++index) {
    if (!((profile[index] >= '0' && profile[index] <= '9') ||
          (profile[index] >= 'a' && profile[index] <= 'f') ||
          (profile[index] >= 'A' && profile[index] <= 'F'))) return 0;
  }
  return 1;
}

int aula_control_peer_is_authorized(const aula_control_server *server,
                                      int descriptor) {
  uint32_t uid;
#if defined(__APPLE__)
  uid_t peer_uid;
  gid_t peer_gid;
  if (getpeereid(descriptor, &peer_uid, &peer_gid) != 0) return 0;
  (void)peer_gid;
  uid = (uint32_t)peer_uid;
#elif defined(__linux__)
  struct ucred credentials;
  socklen_t length = (socklen_t)sizeof(credentials);
  if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
      length != (socklen_t)sizeof(credentials)) return 0;
  uid = (uint32_t)credentials.uid;
#else
  (void)server;
  (void)descriptor;
  return 0;
#endif
  return uid == server->config.authorized_gateway_uid;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
static int append_json(char *response, size_t capacity, size_t *length,
                       const char *format, ...) {
  int written;
  va_list arguments;
  if (*length >= capacity) return 0;
  va_start(arguments, format);
  written = vsnprintf(response + *length, capacity - *length, format, arguments);
  va_end(arguments);
  if (written < 0 || (size_t)written >= capacity - *length) return 0;
  *length += (size_t)written;
  return 1;
}
#pragma clang diagnostic pop

static int format_status(const aula_endpoint_status *status,
                         char *response, size_t capacity, size_t *out_length) {
  size_t length = 0U;
  int ok = append_json(response, capacity, &length,
      "{\"call_state\":\"%s\",\"media_state\":\"%s\","
      "\"revision\":1,\"rx_rendering\":%s,\"video_transmit_enabled\":%s,\"audio_muted\":%s,\"reconnect_attempts\":%u,"
      "\"renderer\":{\"healthy\":%s,\"audio\":{\"rendering\":%s,\"fresh\":%s,\"last_success_ns\":%llu},\"video\":{\"rendering\":%s,\"fresh\":%s,\"last_success_ns\":%llu}},"
      "\"aec\":{\"delay_state\":\"%s\",\"available\":%s,\"active\":%s,\"processed_frames\":%llu,\"reference_underflows\":%llu,\"resets\":%llu},"
      "\"sip\":{\"profile\":\"%s\",\"transport\":\"%s\","
      "\"registration\":\"%s\",\"status_code\":%u,\"expires_seconds\":%u},"
      "\"video\":{\"payload_type\":%u,\"direction\":\"%s\","
      "\"security\":\"%s\",\"h264_profile_level_id\":\"%s\",\"frames\":%llu,\"packets\":%llu,"
      "\"lost\":%llu,\"jitter_ns\":%llu},"
      "\"audio\":{\"payload_type\":%u,\"direction\":\"%s\","
      "\"security\":\"%s\",\"frames\":%llu,\"packets\":%llu,\"lost\":%llu,\"jitter_ns\":%llu},"
      "\"last_error\":%d}",
      call_state_name(status->call_state), status_media_state(status),
      json_bool(status->rx_rendering), json_bool(status->video_transmit_enabled),
      json_bool(status->audio_muted),
      status->reconnect_attempts,
      json_bool(status->rx_renderer_healthy), json_bool(status->rx_audio_rendering),
      json_bool(status->rx_audio_render_fresh),
      (unsigned long long)status->rx_audio_last_success_ns,
      json_bool(status->rx_video_rendering), json_bool(status->rx_video_render_fresh),
      (unsigned long long)status->rx_video_last_success_ns,
      status_aec_state(status), json_bool(status->aec.available),
      json_bool(status->aec.active),
      (unsigned long long)status->aec.processed_frames,
      (unsigned long long)status->aec.reference_underflows,
      (unsigned long long)status->aec.resets, status_sip_profile(status),
      status_sip_transport(status), status_registration(status),
      (unsigned int)status->registration.status_code,
      (unsigned int)status->registration.expires_seconds,
      (unsigned int)status->video_payload_type,
      direction_name(status->video_direction), status->video_srtp != 0 ? "srtp" : "rtp",
      status_profile(status),
      (unsigned long long)status->video_backend.frames_read,
      (unsigned long long)status->video_rtp.packets,
      (unsigned long long)status->video_rtp.lost,
      (unsigned long long)status->video_rtp.jitter_ns,
      (unsigned int)status->audio_payload_type,
      direction_name(status->audio_direction), status->audio_srtp != 0 ? "srtp" : "rtp",
      (unsigned long long)status->audio_backend.frames_read,
      (unsigned long long)status->audio_rtp.packets,
      (unsigned long long)status->audio_rtp.lost,
      (unsigned long long)status->audio_rtp.jitter_ns,
      (int)status->last_error);
  if (ok) *out_length = length;
  return ok;
}

static aula_control_client *control_client_for_descriptor(
    aula_control_server *server, int descriptor) {
  size_t index;
  if (server == NULL) return NULL;
  for (index = 0U; index < AULA_CONTROL_CLIENT_LIMIT; ++index) {
    if (server->clients[index].descriptor == descriptor) return &server->clients[index];
  }
  return NULL;
}

static void send_frame(aula_control_server *server, int descriptor,
                       aula_control_opcode opcode,
                       uint16_t flags, uint32_t request_id,
                       const uint8_t *payload, size_t payload_length) {
  aula_control_client *client = control_client_for_descriptor(server, descriptor);
  aula_control_frame frame;
  aula_mutable_bytes encoded;
  if (client == NULL || client->response_length != 0U) return;
  encoded.data = client->response;
  encoded.capacity = sizeof(client->response);
  encoded.length = 0U;
  (void)memset(&frame, 0, sizeof(frame));
  frame.opcode = opcode;
  frame.flags = flags;
  frame.request_id = request_id;
  frame.payload.data = payload;
  frame.payload.length = payload_length;
  if (aula_control_frame_encode(&frame, &encoded) == AULA_STATUS_OK)
    client->response_length = encoded.length;
}

static void send_error(aula_control_server *server, int descriptor,
                       aula_control_opcode opcode,
                       uint32_t request_id, const char *code) {
  char payload[160];
  int length = snprintf(payload, sizeof(payload),
      "{\"ok\":false,\"error\":{\"code\":\"%s\"}}", code);
  if (length > 0 && (size_t)length < sizeof(payload))
    send_frame(server, descriptor, opcode,
               AULA_CONTROL_FRAME_RESPONSE | AULA_CONTROL_FRAME_ERROR,
               request_id, (const uint8_t *)payload, (size_t)length);
}

static int opcode_is_idempotent_command(aula_control_opcode opcode) {
  return opcode >= AULA_CONTROL_OPCODE_ORIGINATE &&
      opcode <= AULA_CONTROL_OPCODE_SETTINGS;
}

static int request_is_settings_read(const aula_control_frame *request) {
  return request->opcode == AULA_CONTROL_OPCODE_SETTINGS &&
      aula_control_payload_is_empty_object(request->payload);
}

static int request_is_cacheable_command(const aula_control_frame *request) {
  return opcode_is_idempotent_command(request->opcode) &&
      !request_is_settings_read(request);
}

static aula_control_completed_request *find_completed(
    aula_control_server *server, const aula_control_frame *request,
    int *conflict) {
  size_t index;
  uint64_t fingerprint[2];
  *conflict = 0;
  aula_control_request_fingerprint(server->completed_cache_key,
                                    request->payload, fingerprint);
  for (index = 0U; index < AULA_CONTROL_COMPLETED_CACHE_ENTRIES; ++index) {
    aula_control_completed_request *entry = &server->completed[index];
    if (!entry->used || entry->request_id != request->request_id) continue;
    if (entry->opcode == request->opcode &&
        entry->request_length == request->payload.length &&
        entry->request_fingerprint[0] == fingerprint[0] &&
        entry->request_fingerprint[1] == fingerprint[1]) return entry;
    *conflict = 1;
    return NULL;
  }
  return NULL;
}

static int cache_completed(aula_control_server *server,
                           const aula_control_frame *request,
                           uint16_t response_flags, const uint8_t *payload,
                           size_t payload_length) {
  aula_control_completed_request *entry;
  if (request->payload.length > AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES ||
      payload_length > AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES) return 0;
  entry = &server->completed[
      server->next_completed++ % AULA_CONTROL_COMPLETED_CACHE_ENTRIES];
  (void)memset(entry, 0, sizeof(*entry));
  entry->used = 1;
  entry->opcode = request->opcode;
  entry->request_id = request->request_id;
  entry->request_length = request->payload.length;
  entry->response_length = payload_length;
  entry->response_flags = response_flags;
  aula_control_request_fingerprint(server->completed_cache_key,
                                    request->payload,
                                    entry->request_fingerprint);
  if (payload_length != 0U)
    (void)memcpy(entry->response_payload, payload, payload_length);
  return 1;
}

static int replay_completed(aula_control_server *server, int descriptor,
                            const aula_control_frame *request) {
  aula_control_completed_request *entry;
  int conflict;
  if (!request_is_cacheable_command(request)) return 0;
  entry = find_completed(server, request, &conflict);
  if (conflict) {
    send_error(server, descriptor, request->opcode, request->request_id,
               "REQUEST_ID_CONFLICT");
    return 1;
  }
  if (entry == NULL) return 0;
  send_frame(server, descriptor, entry->opcode, entry->response_flags,
             entry->request_id, entry->response_payload,
             entry->response_length);
  return 1;
}

static void complete_command(aula_control_server *server, int descriptor,
                             const aula_control_frame *request,
                             uint16_t response_flags, const uint8_t *payload,
                             size_t payload_length) {
  if (!request_is_cacheable_command(request)) {
    send_frame(server, descriptor, request->opcode, response_flags,
               request->request_id, payload, payload_length);
    return;
  }
  if (!cache_completed(server, request, response_flags, payload, payload_length)) {
    send_error(server, descriptor, request->opcode, request->request_id, "INTERNAL");
    return;
  }
  /* Cache before delivery so a lost reply can be recovered without replaying
   * the already completed command. */
  send_frame(server, descriptor, request->opcode, response_flags,
             request->request_id, payload, payload_length);
}

static const char *command_error_code(aula_status status) {
  if (status == AULA_STATUS_CONFLICT) return "REVISION_CONFLICT";
  if (status == AULA_STATUS_PERSISTENCE_UNCERTAIN)
    return "PERSISTENCE_UNCERTAIN";
  if (status == AULA_STATUS_PERMISSION_DENIED) return "PERMISSION_DENIED";
  if (status == AULA_STATUS_UNSUPPORTED) return "UNSUPPORTED";
  return "COMMAND_FAILED";
}

static int request_is_empty_status(const aula_control_frame *request) {
  return request->payload.length == 0U ||
      (request->payload.length == 2U && request->payload.data != NULL &&
       memcmp(request->payload.data, "{}", 2U) == 0);
}

static void dispatch_status_request(aula_control_server *server, int descriptor,
                                    const aula_control_frame *request,
                                    aula_mutable_bytes *response) {
  if (!request_is_empty_status(request)) {
    send_error(server, descriptor, request->opcode, request->request_id, "INVALID_REQUEST");
    return;
  }
  if (!format_status(&server->status, (char *)response->data, response->capacity,
                     &response->length)) {
    send_error(server, descriptor, request->opcode, request->request_id, "INTERNAL");
    return;
  }
  send_frame(server, descriptor, request->opcode, AULA_CONTROL_FRAME_RESPONSE,
             request->request_id, response->data, response->length);
}

static aula_status dispatch_command_request(aula_control_server *server,
                                             const aula_control_frame *request,
                                             aula_mutable_bytes *response) {
  if (request->opcode != AULA_CONTROL_OPCODE_HANGUP) {
    return server->config.request_callback == NULL
        ? AULA_STATUS_UNSUPPORTED
        : server->config.request_callback(server->config.command_context, request,
                                          response);
  }
  if (!aula_control_payload_is_empty_object(request->payload)) {
    return AULA_STATUS_INVALID_DATA;
  }
  if (server->config.request_callback != NULL) {
    return server->config.request_callback(server->config.command_context, request,
                                           response);
  }
  return server->config.command_callback == NULL
      ? AULA_STATUS_STATE_ERROR
      : server->config.command_callback(server->config.command_context,
                                        AULA_CONTROL_HANGUP);
}

static void dispatch_request(aula_control_server *server, int descriptor,
                             const aula_control_frame *request) {
  uint8_t payload[AULA_CONTROL_MAX_PAYLOAD_BYTES];
  aula_mutable_bytes response = {payload, sizeof(payload), 0U};
  aula_status status;
  if (request->flags != 0U) {
    send_error(server, descriptor, request->opcode, request->request_id, "INVALID_FLAGS");
    return;
  }
  if (replay_completed(server, descriptor, request)) return;
  if (request->opcode == AULA_CONTROL_OPCODE_STATUS) {
    dispatch_status_request(server, descriptor, request, &response);
    return;
  }
  if (request->opcode == AULA_CONTROL_OPCODE_HANGUP &&
      !aula_control_payload_is_empty_object(request->payload)) {
    send_error(server, descriptor, request->opcode, request->request_id, "INVALID_REQUEST");
    return;
  }
  status = dispatch_command_request(server, request, &response);
  if (status != AULA_STATUS_OK) {
    if (status == AULA_STATUS_PERSISTENCE_UNCERTAIN) {
      static const uint8_t uncertain[] =
          "{\"ok\":false,\"error\":{\"code\":\"PERSISTENCE_UNCERTAIN\"}}";
      complete_command(server, descriptor, request,
                       AULA_CONTROL_FRAME_RESPONSE | AULA_CONTROL_FRAME_ERROR,
                       uncertain, sizeof(uncertain) - 1U);
      return;
    }
    send_error(server, descriptor, request->opcode, request->request_id,
               command_error_code(status));
    return;
  }
  if (response.length > response.capacity) {
    send_error(server, descriptor, request->opcode, request->request_id, "INTERNAL");
    return;
  }
  if (response.length == 0U) {
    static const uint8_t ok[] = "{\"ok\":true}";
    complete_command(server, descriptor, request, AULA_CONTROL_FRAME_RESPONSE,
                     ok, sizeof(ok) - 1U);
  } else {
    complete_command(server, descriptor, request, AULA_CONTROL_FRAME_RESPONSE,
                     response.data,
                     response.length);
  }
}

static aula_status control_receive_request_bytes(
    const aula_control_server *server, aula_control_client *client) {
  struct iovec iov;
  struct msghdr message;
  ssize_t received;
  if (client->request_length >= AULA_CONTROL_HEADER_BYTES +
                                   server->config.maximum_request_bytes) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  (void)memset(&message, 0, sizeof(message));
  iov.iov_base = client->request + client->request_length;
  iov.iov_len = AULA_CONTROL_HEADER_BYTES + server->config.maximum_request_bytes -
                client->request_length;
  message.msg_iov = &iov;
  message.msg_iovlen = 1U;
  received = recvmsg(client->descriptor, &message, 0);
  if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return AULA_STATUS_AGAIN;
  }
  if (received == 0) return AULA_STATUS_END;
  if (received < 0) return AULA_STATUS_IO_ERROR;
  if ((message.msg_flags & MSG_TRUNC) != 0) return AULA_STATUS_LIMIT_EXCEEDED;
  client->request_length += (size_t)received;
  return AULA_STATUS_OK;
}

static aula_status control_incomplete_frame(void) {
#if defined(__APPLE__)
  return AULA_STATUS_AGAIN; /* Host-only stream compatibility. */
#else
  return AULA_STATUS_INVALID_DATA; /* LSZ1 occupies one seqpacket message. */
#endif
}

static aula_status control_complete_request(
    const aula_control_server *server, const aula_control_client *client,
    aula_bytes *out_input) {
  uint32_t payload_length;
  size_t expected_length;
  if (client->request_length < AULA_CONTROL_HEADER_BYTES)
    return control_incomplete_frame();
  (void)memcpy(&payload_length, client->request + 12U, sizeof(payload_length));
  payload_length = ntohl(payload_length);
  if (payload_length > server->config.maximum_request_bytes) {
    return AULA_STATUS_LIMIT_EXCEEDED;
  }
  expected_length = AULA_CONTROL_HEADER_BYTES + (size_t)payload_length;
  if (client->request_length < expected_length) return control_incomplete_frame();
  if (client->request_length != expected_length) return AULA_STATUS_INVALID_DATA;
  out_input->data = client->request;
  out_input->length = client->request_length;
  return AULA_STATUS_OK;
}

aula_status aula_control_handle_client(aula_control_server *server,
                                         aula_control_client *client) {
  aula_control_frame request;
  aula_bytes input;
  aula_status status;
  if (server == NULL || client == NULL || client->descriptor < 0)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (!aula_control_peer_is_authorized(server, client->descriptor))
    return AULA_STATUS_PERMISSION_DENIED;
  status = control_receive_request_bytes(server, client);
  if (status != AULA_STATUS_OK) return status;
  status = control_complete_request(server, client, &input);
  if (status != AULA_STATUS_OK) return status;
  if (aula_control_frame_decode(input, &request) != AULA_STATUS_OK ||
      request.payload.length > server->config.maximum_request_bytes) {
    return AULA_STATUS_INVALID_DATA;
  }
  dispatch_request(server, client->descriptor, &request);
  protocol_secure_zero(client->request, client->request_length);
  client->request_length = 0U;
  (void)memset(&request, 0, sizeof(request));
  return client->response_length == 0U ? AULA_STATUS_INTERNAL_ERROR :
                                        AULA_STATUS_OK;
}
