#ifndef LS200_SIPD_LOG_H
#define LS200_SIPD_LOG_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_log_level {
  LS200_LOG_DEBUG = 0,
  LS200_LOG_INFO = 1,
  LS200_LOG_NOTICE = 2,
  LS200_LOG_WARNING = 3,
  LS200_LOG_ERROR = 4
} ls200_log_level;

typedef enum ls200_log_sink {
  LS200_LOG_SINK_STDERR = 0,
  LS200_LOG_SINK_SYSLOG = 1
} ls200_log_sink;

typedef struct ls200_log_event {
  ls200_log_level level;
  const char *component;
  const char *event;
  const char *correlation_id;
  const char *call_state;
  const char *reason_code;
  uint64_t monotonic_ns;
  int rx_rendering;
} ls200_log_event;

typedef struct ls200_log_config {
  ls200_log_sink sink;
  ls200_log_level minimum_level;
  uint32_t maximum_events_per_interval;
  uint32_t interval_seconds;
} ls200_log_config;

typedef struct ls200_logger ls200_logger;

ls200_status ls200_log_create(const ls200_log_config *config, ls200_logger **out_logger);
/* Event fields are schema tokens, not arbitrary text. correlation_id must be
 * the daemon-generated 32-character lowercase hexadecimal identifier. */
ls200_status ls200_log_write(ls200_logger *logger, const ls200_log_event *event);
/* redacted_value is restricted to the logger's fixed redaction/status tokens. */
ls200_status ls200_log_write_field(ls200_logger *logger, const ls200_log_event *event,
                                   const char *key, const char *redacted_value);
void ls200_log_destroy(ls200_logger *logger);

#ifdef __cplusplus
}
#endif

#endif
