#include "server.h"
#include "credentials.h"
#include "projection.h"
#include "transport.h"
#include "wire.h"
#include <string.h>
#include <unistd.h>

static int revision_one_request(json_t *request) {
  const char *operation = json_string_value(json_object_get(request, "operation"));
  return json_is_object(request) && json_object_size(request) == 2 &&
      json_is_integer(json_object_get(request, "revision")) &&
      json_integer_value(json_object_get(request, "revision")) == 1 &&
      operation != NULL && strcmp(operation, "status") == 0;
}

static json_t *status_response(ls200_device_read_fn read_status) {
  json_t *raw = read_status();
  json_t *response = ls200_device_project_recorder(raw);
  json_decref(raw);
  return response;
}

static int query_request_valid(json_t *request) {
  const char *correlation = json_string_value(json_object_get(request, "correlation"));
  json_t *revision = json_object_get(request, "revision");
  return json_is_object(request) && json_object_size(request) == 4 &&
      json_is_integer(revision) && json_integer_value(revision) == 2 &&
      correlation != NULL && strlen(correlation) > 0 && strlen(correlation) <= 64 &&
      strspn(correlation, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == strlen(correlation) &&
      json_is_object(json_object_get(request, "arguments")) &&
      json_is_string(json_object_get(request, "operation"));
}

static int job_query_valid(json_t *arguments) {
  json_t *offset = json_object_get(arguments, "offset");
  return json_object_size(arguments) == 1 && json_is_integer(offset) &&
      json_integer_value(offset) >= 0 && json_integer_value(offset) <= 32;
}

static json_t *job_page(ls200_device_read_fn read_jobs, size_t offset) {
  json_t *all = read_jobs == NULL ? NULL : read_jobs();
  json_t *page = json_array(), *result = NULL;
  size_t end = offset + 8;
  if (!json_is_array(all) || json_array_size(all) > 32 || page == NULL) goto done;
  if (end > json_array_size(all)) end = json_array_size(all);
  for (size_t i = offset; i < end; ++i)
    if (json_array_append(page, json_array_get(all, i)) != 0) goto done;
  result = json_pack("{s:O,s:o}", "jobs", page, "next",
      end < json_array_size(all) ? json_integer((json_int_t)end) : json_null());
done:
  json_decref(all);
  json_decref(page);
  return result;
}

static json_t *query_response(json_t *request, ls200_device_read_fn read_status,
    ls200_device_read_fn read_jobs, ls200_device_dispatch_fn dispatch) {
  json_t *data = NULL, *arguments, *response;
  const char *operation, *outcome = NULL;
  if (!query_request_valid(request)) return NULL;
  arguments = json_object_get(request, "arguments");
  operation = json_string_value(json_object_get(request, "operation"));
  if (strcmp(operation, "status") == 0 && json_object_size(arguments) == 0)
    data = status_response(read_status);
  else if (strcmp(operation, "jobs.list") == 0 && job_query_valid(arguments))
    data = job_page(read_jobs, (size_t)json_integer_value(json_object_get(arguments, "offset")));
  else if (dispatch != NULL &&
      ((strcmp(operation, "credentials.status") == 0 && json_object_size(arguments) == 0) ||
       (strcmp(operation, "credentials.replace") == 0 && ls200_device_credentials_schema(arguments))))
    data = dispatch(operation, arguments, json_string_value(json_object_get(request, "correlation")), &outcome);
  else return NULL;
  if (outcome == NULL) outcome = data == NULL ? "unavailable" : "succeeded";
  response = json_pack("{s:i,s:s,s:s,s:s,s:O}", "revision", 2,
      "correlation", json_string_value(json_object_get(request, "correlation")),
      "operation", operation, "outcome", outcome, "data", data == NULL ? json_null() : data);
  json_decref(data);
  return response;
}

int ls200_device_serve_commands(int fd, uint32_t peer_uid, ls200_device_read_fn read_status,
    ls200_device_read_fn read_jobs, ls200_device_dispatch_fn dispatch) {
  uint64_t deadline = ls200_device_clock() + 4000;
  json_t *request, *response;
  int result;
  if (read_status == NULL || !ls200_device_peer(fd, peer_uid)) return 0;
  request = ls200_device_receive_message(fd, deadline);
  response = revision_one_request(request) ? status_response(read_status) :
      query_response(request, read_status, read_jobs, dispatch);
  json_decref(request);
  result = response != NULL && ls200_device_send_message(fd, response, deadline);
  json_decref(response);
  return result;
}

int ls200_device_serve_queries(int fd, ls200_device_read_fn read_status,
    ls200_device_read_fn read_jobs) {
  return ls200_device_serve_commands(fd, (uint32_t)geteuid(), read_status, read_jobs, NULL);
}

int ls200_device_serve_request(int fd, ls200_device_read_fn read_status) {
  return ls200_device_serve_queries(fd, read_status, NULL);
}
