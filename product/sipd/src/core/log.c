#define _POSIX_C_SOURCE 200809L

#include "ls200_sipd/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define LS200_LOG_LINE_BYTES 1024U
#define LS200_LOG_FIELD_BYTES 96U

struct ls200_logger {
  ls200_log_config config;
  uint64_t interval_start_ns;
  uint32_t emitted;
  uint32_t suppressed;
  int syslog_open;
};

static uint64_t logger_now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
  return ((uint64_t)now.tv_sec * UINT64_C(1000000000)) + (uint64_t)now.tv_nsec;
}

static int contains_sensitive_marker(const char *value) {
  static const char *const markers[] = {"pass", "password", "meeting", "hostkey", "digest", "authorization", "token", "cookie", "call-id", "callid", "secret", "bearer", "sip"};
  size_t marker_index;
  char folded[LS200_LOG_FIELD_BYTES];
  size_t length = 0U;
  while (value[length] != '\0' && length + 1U < sizeof(folded)) {
    unsigned char character = (unsigned char)value[length];
    folded[length] = (char)((character >= (unsigned char)'A' && character <= (unsigned char)'Z') ? character + ((unsigned char)'a' - (unsigned char)'A') : character);
    ++length;
  }
  folded[length] = '\0';
  for (marker_index = 0U; marker_index < sizeof(markers) / sizeof(markers[0]); ++marker_index) if (strstr(folded, markers[marker_index]) != NULL) return 1;
  return 0;
}

static int is_field_character(unsigned char character) {
  return (character >= (unsigned char)'a' && character <= (unsigned char)'z') ||
         (character >= (unsigned char)'A' && character <= (unsigned char)'Z') ||
         (character >= (unsigned char)'0' && character <= (unsigned char)'9') ||
         character == (unsigned char)'_' || character == (unsigned char)'-' ||
         character == (unsigned char)':';
}

static int is_long_numeric_field(const char *value, size_t length) {
  size_t index;
  if (length < 8U) return 0;
  for (index = 0U; index < length; ++index)
    if (value[index] < '0' || value[index] > '9') return 0;
  return 1;
}

static const char *level_name(ls200_log_level level);

static int is_safe_field(const char *value, int required) {
  const char *start = value;
  size_t length = 0U;
  if (value == NULL) return required ? 0 : 1;
  while (*value != '\0') {
    unsigned char character = (unsigned char)*value;
    if (!is_field_character(character)) return 0;
    ++length;
    if (length >= LS200_LOG_FIELD_BYTES) return 0;
    ++value;
  }
  if (is_long_numeric_field(start, length)) return 0;
  if (contains_sensitive_marker(start)) return 0;
  return required ? (length != 0U) : 1;
}

static int format_event_line(char *line, size_t capacity, const ls200_log_event *event,
                             uint64_t now, const char *field_key,
                             const char *field_value) {
  const char *correlation = event->correlation_id == NULL ? "-" : event->correlation_id;
  const char *state = event->call_state == NULL ? "-" : event->call_state;
  const char *reason = event->reason_code == NULL ? "-" : event->reason_code;
  if (field_key == NULL)
    return snprintf(line, capacity,
                    "{\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"correlation_id\":\"%s\",\"call_state\":\"%s\",\"reason_code\":\"%s\",\"monotonic_ns\":%llu,\"rx_rendering\":false}",
                    level_name(event->level), event->component, event->event,
                    correlation, state, reason, (unsigned long long)now);
  return snprintf(line, capacity,
                  "{\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"correlation_id\":\"%s\",\"call_state\":\"%s\",\"reason_code\":\"%s\",\"monotonic_ns\":%llu,\"%s\":\"%s\",\"rx_rendering\":false}",
                  level_name(event->level), event->component, event->event,
                  correlation, state, reason, (unsigned long long)now,
                  field_key, field_value);
}

static int is_one_of(const char *value, const char *const *allowed,
                     size_t allowed_count, int required) {
  size_t index;
  if (value == NULL) return required ? 0 : 1;
  for (index = 0U; index < allowed_count; ++index)
    if (strcmp(value, allowed[index]) == 0) return 1;
  return 0;
}

static int is_correlation_id(const char *value) {
  size_t index;
  if (value == NULL) return 1;
  if (strlen(value) != 32U) return 0;
  for (index = 0U; index < 32U; ++index)
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) return 0;
  return 1;
}

static const char *level_name(ls200_log_level level) {
  switch (level) {
    case LS200_LOG_DEBUG: return "debug";
    case LS200_LOG_INFO: return "info";
    case LS200_LOG_NOTICE: return "notice";
    case LS200_LOG_WARNING: return "warning";
    case LS200_LOG_ERROR: return "error";
  }
  return "invalid";
}

static int syslog_priority(ls200_log_level level) {
  switch (level) {
    case LS200_LOG_DEBUG: return LOG_DEBUG;
    case LS200_LOG_INFO: return LOG_INFO;
    case LS200_LOG_NOTICE: return LOG_NOTICE;
    case LS200_LOG_WARNING: return LOG_WARNING;
    case LS200_LOG_ERROR: return LOG_ERR;
  }
  return LOG_ERR;
}

static ls200_status emit_line(ls200_logger *logger, const char *line, ls200_log_level level) {
  if (logger->config.sink == LS200_LOG_SINK_SYSLOG) {
    syslog(syslog_priority(level), "%s", line);
  } else if (fputs(line, stderr) < 0 || fputc('\n', stderr) == EOF) {
    return LS200_STATUS_IO_ERROR;
  }
  return LS200_STATUS_OK;
}

static ls200_status emit_summary(ls200_logger *logger) {
  char line[LS200_LOG_LINE_BYTES];
  int written;
  if (logger->suppressed == 0U) return LS200_STATUS_OK;
  written = snprintf(line, sizeof(line),
                     "{\"level\":\"notice\",\"component\":\"logger\",\"event\":\"rate_summary\",\"suppressed\":%u,\"rx_rendering\":false}",
                     logger->suppressed);
  if (written < 0 || (size_t)written >= sizeof(line)) return LS200_STATUS_INTERNAL_ERROR;
  logger->suppressed = 0U;
  return emit_line(logger, line, LS200_LOG_NOTICE);
}

static ls200_status prepare_interval(ls200_logger *logger, uint64_t now) {
  uint64_t duration;
  if (now == 0U) return LS200_STATUS_IO_ERROR;
  duration = (uint64_t)logger->config.interval_seconds * UINT64_C(1000000000);
  if (logger->interval_start_ns == 0U) {
    logger->interval_start_ns = now;
    return LS200_STATUS_OK;
  }
  if (now - logger->interval_start_ns >= duration) {
    ls200_status status = emit_summary(logger);
    logger->interval_start_ns = now;
    logger->emitted = 0U;
    return status;
  }
  return LS200_STATUS_OK;
}

static ls200_status validate_event(const ls200_log_event *event) {
  static const char *const components[] = {"core", "endpoint", "logger", "sip", "sdp",
    "rtp", "rtcp", "media", "backend", "platform", "control"};
  static const char *const events[] = {"state", "created", "destroyed", "event_failure",
    "rate_summary", "invite_accept_failed", "media_target_rejected"};
  static const char *const states[] = {"idle", "resolving", "inviting", "early",
    "establishing_media", "established", "terminating", "backing_off", "failed",
    "terminated", "stopped", "terminal_failure"};
  static const char *const reasons[] = {"ok", "local_ready", "transaction_rejected",
    "orderly_stop", "timeout", "transport_error", "media_error", "transient_failure",
    "sdp_negotiation_failed", "missing_sdp_answer", "media_prepare_failed",
    "media_commit_failed", "sip_peer_not_pinned", "external_policy_rejected",
    "sip_peer_address_mismatch"};
  if (event == NULL || event->level < LS200_LOG_DEBUG || event->level > LS200_LOG_ERROR || event->rx_rendering != 0 ||
      !is_one_of(event->component, components, sizeof(components) / sizeof(components[0]), 1) ||
      !is_one_of(event->event, events, sizeof(events) / sizeof(events[0]), 1) ||
      !is_correlation_id(event->correlation_id) ||
      !is_one_of(event->call_state, states, sizeof(states) / sizeof(states[0]), 0) ||
      !is_one_of(event->reason_code, reasons, sizeof(reasons) / sizeof(reasons[0]), 0))
    return LS200_STATUS_SECURITY_ERROR;
  return LS200_STATUS_OK;
}

ls200_status ls200_log_create(const ls200_log_config *config, ls200_logger **out_logger) {
  ls200_logger *logger;
  if (config == NULL || out_logger == NULL || *out_logger != NULL ||
      (config->sink != LS200_LOG_SINK_STDERR && config->sink != LS200_LOG_SINK_SYSLOG) ||
      config->minimum_level < LS200_LOG_DEBUG || config->minimum_level > LS200_LOG_ERROR ||
      config->maximum_events_per_interval == 0U || config->maximum_events_per_interval > 10000U ||
      config->interval_seconds == 0U || config->interval_seconds > 86400U) return LS200_STATUS_INVALID_ARGUMENT;
  logger = (ls200_logger *)calloc(1U, sizeof(*logger));
  if (logger == NULL) return LS200_STATUS_INTERNAL_ERROR;
  logger->config = *config;
  if (config->sink == LS200_LOG_SINK_SYSLOG) { openlog("ls200-sipd", LOG_PID | LOG_NDELAY, LOG_DAEMON); logger->syslog_open = 1; }
  *out_logger = logger;
  return LS200_STATUS_OK;
}

static ls200_status log_write_internal(ls200_logger *logger, const ls200_log_event *event,
                                       const char *field_key, const char *field_value) {
  char line[LS200_LOG_LINE_BYTES];
  uint64_t now;
  int written;
  ls200_status status;
  if (logger == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  status = validate_event(event);
  if (status != LS200_STATUS_OK) return status;
  if (event->level < logger->config.minimum_level) return LS200_STATUS_OK;
  now = event->monotonic_ns == 0U ? logger_now_ns() : event->monotonic_ns;
  status = prepare_interval(logger, now);
  if (status != LS200_STATUS_OK) return status;
  if (logger->emitted >= logger->config.maximum_events_per_interval) { ++logger->suppressed; return LS200_STATUS_AGAIN; }
  written = format_event_line(line, sizeof(line), event, now, field_key, field_value);
  if (written < 0 || (size_t)written >= sizeof(line)) return LS200_STATUS_LIMIT_EXCEEDED;
  ++logger->emitted;
  return emit_line(logger, line, event->level);
}

ls200_status ls200_log_write(ls200_logger *logger, const ls200_log_event *event) {
  return log_write_internal(logger, event, NULL, NULL);
}

ls200_status ls200_log_write_field(ls200_logger *logger, const ls200_log_event *event,
                                   const char *key, const char *redacted_value) {
  static const char *const redacted_tokens[] = {"redacted", "present", "absent",
    "enabled", "disabled", "accepted", "rejected", "unknown"};
  if (logger == NULL || event == NULL || !is_safe_field(key, 1) ||
      !is_one_of(redacted_value, redacted_tokens,
                 sizeof(redacted_tokens) / sizeof(redacted_tokens[0]), 1))
    return LS200_STATUS_SECURITY_ERROR;
  return log_write_internal(logger, event, key, redacted_value);
}

void ls200_log_destroy(ls200_logger *logger) {
  if (logger != NULL) {
    (void)emit_summary(logger);
    if (logger->syslog_open) closelog();
    (void)memset(logger, 0, sizeof(*logger));
    free(logger);
  }
}
