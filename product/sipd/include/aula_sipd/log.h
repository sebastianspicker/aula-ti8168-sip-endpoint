#ifndef AULA_SIPD_LOG_H
#define AULA_SIPD_LOG_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_log_level {
  AULA_LOG_DEBUG = 0,
  AULA_LOG_INFO = 1,
  AULA_LOG_NOTICE = 2,
  AULA_LOG_WARNING = 3,
  AULA_LOG_ERROR = 4
} aula_log_level;

typedef enum aula_log_sink {
  AULA_LOG_SINK_STDERR = 0,
  AULA_LOG_SINK_SYSLOG = 1
} aula_log_sink;

typedef struct aula_log_event {
  aula_log_level level;
  const char *component;
  const char *event;
  const char *correlation_id;
  const char *call_state;
  const char *reason_code;
  uint64_t monotonic_ns;
  int rx_rendering;
} aula_log_event;

typedef struct aula_log_config {
  aula_log_sink sink;
  aula_log_level minimum_level;
  uint32_t maximum_events_per_interval;
  uint32_t interval_seconds;
} aula_log_config;

typedef struct aula_logger aula_logger;

aula_status aula_log_create(const aula_log_config *config, aula_logger **out_logger);
/* Event fields are schema tokens, not arbitrary text. correlation_id must be
 * the daemon-generated 32-character lowercase hexadecimal identifier. */
aula_status aula_log_write(aula_logger *logger, const aula_log_event *event);
/* redacted_value is restricted to the logger's fixed redaction/status tokens. */
aula_status aula_log_write_field(aula_logger *logger, const aula_log_event *event,
                                   const char *key, const char *redacted_value);
void aula_log_destroy(aula_logger *logger);

#ifdef __cplusplus
}
#endif

#endif
