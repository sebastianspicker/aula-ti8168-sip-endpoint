/* LSZ1 request handling: peer authorisation, dispatch, and the
 * completed-command cache. The server state is built directly and each
 * request travels over a connected socketpair, so the test needs no
 * listening socket or filesystem path. One request per connection, as the
 * gateway sends them. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L

#include "aula_sipd/platform.h"
#include "aula_test.h"
#include "control/control_private.h"

#include <sys/socket.h>
#include <unistd.h>

typedef struct test_context {
  unsigned int calls[AULA_CONTROL_OPCODE_METRICS + 1];
  /* When set, replies exceed the completed-command cache entry size. */
  int large_reply;
} test_context;

typedef struct test_reply {
  uint8_t wire[AULA_CONTROL_WIRE_BYTES];
  size_t length;
  aula_control_frame frame;
} test_reply;

static aula_status record_request(void *context,
                                  const aula_control_frame *request,
                                  aula_mutable_bytes *response) {
  test_context *calls = (test_context *)context;
  int length;
  calls->calls[request->opcode] += 1U;
  if (calls->large_reply) {
    size_t size = AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES + 100U;
    if (size > response->capacity) return AULA_STATUS_INTERNAL_ERROR;
    (void)memset(response->data, ' ', size);
    (void)memcpy(response->data, "{\"ok\":true}", 11U);
    response->length = size;
    return AULA_STATUS_OK;
  }
  length = snprintf((char *)response->data, response->capacity,
                    "{\"ok\":true,\"call\":%u}", calls->calls[request->opcode]);
  if (length < 0 || (size_t)length >= response->capacity)
    return AULA_STATUS_INTERNAL_ERROR;
  response->length = (size_t)length;
  return AULA_STATUS_OK;
}

static aula_control_server *create_server(test_context *context,
                                          uint32_t authorized_uid) {
  aula_control_server *server =
      (aula_control_server *)calloc(1U, sizeof(*server));
  aula_mutable_bytes key;
  size_t index;
  if (server == NULL) return NULL;
  server->descriptor = -1;
  for (index = 0U; index < AULA_CONTROL_CLIENT_LIMIT; ++index)
    server->clients[index].descriptor = -1;
  server->config.socket_mode = 0600U;
  server->config.maximum_request_bytes = AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES;
  server->config.maximum_clients = 4U;
  server->config.authorized_gateway_uid = authorized_uid;
  server->config.request_callback = record_request;
  server->config.command_context = context;
  server->status.call_state = AULA_CALL_IDLE;
  key.data = server->completed_cache_key;
  key.capacity = sizeof(server->completed_cache_key);
  key.length = 0U;
  if (aula_platform_random_bytes(&key) != AULA_STATUS_OK) {
    free(server);
    return NULL;
  }
  return server;
}

/* Sends one encoded request over a fresh connection and lets the server
 * handle it. Returns the handler status; the reply is decoded when present. */
static aula_status exchange_flags(aula_control_server *server,
                                  aula_control_opcode opcode, uint16_t flags,
                                  uint32_t request_id, const char *payload,
                                  test_reply *reply) {
  uint8_t request[AULA_CONTROL_HEADER_BYTES + 512U];
  aula_mutable_bytes encoded = {request, sizeof(request), 0U};
  aula_control_client *client = &server->clients[0];
  aula_control_frame frame;
  aula_status status;
  int pair[2];
  (void)memset(reply, 0, sizeof(*reply));
  (void)memset(&frame, 0, sizeof(frame));
  frame.opcode = opcode;
  frame.flags = flags;
  frame.request_id = request_id;
  frame.payload.data = (const uint8_t *)payload;
  frame.payload.length = strlen(payload);
  if (aula_control_frame_encode(&frame, &encoded) != AULA_STATUS_OK ||
      socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    return AULA_STATUS_INTERNAL_ERROR;
  (void)memset(client, 0, sizeof(*client));
  client->descriptor = pair[0];
  if (write(pair[1], request, encoded.length) != (ssize_t)encoded.length) {
    status = AULA_STATUS_IO_ERROR;
  } else {
    status = aula_control_handle_client(server, client);
  }
  if (client->response_length != 0U) {
    (void)memcpy(reply->wire, client->response, client->response_length);
    reply->length = client->response_length;
    if (aula_control_frame_decode((aula_bytes){reply->wire, reply->length},
                                  &reply->frame) != AULA_STATUS_OK)
      status = AULA_STATUS_INVALID_DATA;
  }
  (void)close(pair[0]);
  (void)close(pair[1]);
  (void)memset(client, 0, sizeof(*client));
  client->descriptor = -1;
  return status;
}

static aula_status exchange(aula_control_server *server,
                            aula_control_opcode opcode, uint32_t request_id,
                            const char *payload, test_reply *reply) {
  return exchange_flags(server, opcode, 0U, request_id, payload, reply);
}

static int reply_is(const test_reply *reply, const char *expected) {
  return reply->frame.payload.length == strlen(expected) &&
      memcmp(reply->frame.payload.data, expected, strlen(expected)) == 0;
}

static int reply_contains(const test_reply *reply, const char *expected) {
  char text[AULA_CONTROL_WIRE_BYTES + 1U];
  size_t length = reply->frame.payload.length;
  (void)memcpy(text, reply->frame.payload.data, length);
  text[length] = '\0';
  return strstr(text, expected) != NULL;
}

static void test_status_is_served_from_published_snapshot(
    aula_control_server *server, test_context *context) {
  test_reply reply;
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_STATUS, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(reply.frame.opcode == AULA_CONTROL_OPCODE_STATUS);
  AULA_CHECK(reply.frame.flags == AULA_CONTROL_FRAME_RESPONSE);
  AULA_CHECK(reply_contains(&reply, "\"idle\""));
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_STATUS] == 0U);
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_STATUS, 0U, "{\"x\":1}",
                             &reply), AULA_STATUS_OK);
  AULA_CHECK((reply.frame.flags & AULA_CONTROL_FRAME_ERROR) != 0U);
  AULA_CHECK(reply_contains(&reply, "INVALID_REQUEST"));
}

static void test_mutation_is_executed_once_per_request_id(
    aula_control_server *server, test_context *context) {
  test_reply first;
  test_reply retry;
  test_reply conflict;
  static const char payload[] = "{\"digits\":\"1\"}";
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 41U, payload, &first),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&first, "{\"ok\":true,\"call\":1}"));
  /* A retried request with the same identifier and payload is answered from
   * the completed-command cache without executing the command again. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 41U, payload, &retry),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&retry, "{\"ok\":true,\"call\":1}"));
  AULA_CHECK(retry.frame.request_id == 41U);
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == 1U);
  /* Reusing the identifier for a different command is a conflict. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 41U,
                             "{\"digits\":\"2\"}", &conflict), AULA_STATUS_OK);
  AULA_CHECK((conflict.frame.flags & AULA_CONTROL_FRAME_ERROR) != 0U);
  AULA_CHECK(reply_contains(&conflict, "REQUEST_ID_CONFLICT"));
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == 1U);
  /* A fresh identifier executes again. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 42U, payload, &first),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&first, "{\"ok\":true,\"call\":2}"));
}

static void test_reads_are_never_served_from_the_command_cache(
    aula_control_server *server, test_context *context) {
  test_reply reply;
  /* The gateway sends reads with request ID 0 and the same "{}" payload on
   * every poll; each poll must observe current state, not a cached reply. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_SUBSCRIBE, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&reply, "{\"ok\":true,\"call\":1}"));
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_SUBSCRIBE, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&reply, "{\"ok\":true,\"call\":2}"));
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_SUBSCRIBE] == 2U);
  /* A different read with the same request ID is not an ID conflict. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DIAGNOSTICS, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&reply, "{\"ok\":true,\"call\":1}"));
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DIAGNOSTICS, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(reply_is(&reply, "{\"ok\":true,\"call\":2}"));
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_SETTINGS, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_SETTINGS, 0U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_SETTINGS] == 2U);
}

static void test_completed_commands_expire(aula_control_server *server,
                                           test_context *context) {
  test_reply reply;
  size_t index;
  unsigned int before = context->calls[AULA_CONTROL_OPCODE_DTMF];
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 60U,
                             "{\"digits\":\"3\"}", &reply), AULA_STATUS_OK);
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == before + 1U);
  /* Age every retained entry past the replay window. */
  for (index = 0U; index < AULA_CONTROL_COMPLETED_CACHE_ENTRIES; ++index)
    server->completed[index].expires_at_ns = 0U;
  /* The same identifier now executes again, and reusing it for another
   * payload is no longer a conflict. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 60U,
                             "{\"digits\":\"3\"}", &reply), AULA_STATUS_OK);
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == before + 2U);
  for (index = 0U; index < AULA_CONTROL_COMPLETED_CACHE_ENTRIES; ++index)
    server->completed[index].expires_at_ns = 0U;
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_DTMF, 60U,
                             "{\"digits\":\"4\"}", &reply), AULA_STATUS_OK);
  AULA_CHECK((reply.frame.flags & AULA_CONTROL_FRAME_ERROR) == 0U);
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == before + 3U);
}

static void test_oversized_reply_is_delivered_but_not_replayed(
    aula_control_server *server, test_context *context) {
  test_reply reply;
  unsigned int before = context->calls[AULA_CONTROL_OPCODE_MEDIA];
  context->large_reply = 1;
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_MEDIA, 70U,
                             "{\"action\":\"mute\"}", &reply), AULA_STATUS_OK);
  AULA_CHECK(reply.frame.flags == AULA_CONTROL_FRAME_RESPONSE);
  AULA_CHECK(reply.frame.payload.length == AULA_SIPD_MAX_CONTROL_MESSAGE_BYTES + 100U);
  /* A retry must not execute the command a second time. */
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_MEDIA, 70U,
                             "{\"action\":\"mute\"}", &reply), AULA_STATUS_OK);
  AULA_CHECK((reply.frame.flags & AULA_CONTROL_FRAME_ERROR) != 0U);
  AULA_CHECK(reply_contains(&reply, "REPLAY_UNAVAILABLE"));
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_MEDIA] == before + 1U);
  context->large_reply = 0;
}

static void test_request_flags_are_rejected(aula_control_server *server,
                                            test_context *context) {
  test_reply reply;
  AULA_CHECK_STATUS(exchange_flags(server, AULA_CONTROL_OPCODE_DTMF,
                                   AULA_CONTROL_FRAME_RESPONSE, 77U, "{}", &reply),
                    AULA_STATUS_OK);
  AULA_CHECK((reply.frame.flags & AULA_CONTROL_FRAME_ERROR) != 0U);
  AULA_CHECK(reply_contains(&reply, "INVALID_FLAGS"));
  AULA_CHECK(context->calls[AULA_CONTROL_OPCODE_DTMF] == 2U);
}

static void test_unauthorized_peer_is_refused(test_context *context) {
  aula_control_server *server = create_server(context, (uint32_t)getuid() + 1U);
  test_reply reply;
  AULA_CHECK(server != NULL);
  if (server == NULL) return;
  AULA_CHECK_STATUS(exchange(server, AULA_CONTROL_OPCODE_STATUS, 0U, "{}", &reply),
                    AULA_STATUS_PERMISSION_DENIED);
  AULA_CHECK(reply.length == 0U);
  free(server);
}

int main(void) {
  test_context context;
  aula_control_server *server;
  (void)memset(&context, 0, sizeof(context));
  server = create_server(&context, (uint32_t)getuid());
  if (server == NULL) {
    (void)fprintf(stderr, "test_control_protocol: cannot create server\n");
    return EXIT_FAILURE;
  }
  test_status_is_served_from_published_snapshot(server, &context);
  test_mutation_is_executed_once_per_request_id(server, &context);
  test_request_flags_are_rejected(server, &context);
  test_reads_are_never_served_from_the_command_cache(server, &context);
  test_completed_commands_expire(server, &context);
  test_oversized_reply_is_delivered_but_not_replayed(server, &context);
  test_unauthorized_peer_is_refused(&context);
  free(server);
  return aula_test_result("test_control_protocol");
}
