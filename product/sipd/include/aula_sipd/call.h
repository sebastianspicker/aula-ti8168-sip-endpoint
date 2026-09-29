#ifndef AULA_SIPD_CALL_H
#define AULA_SIPD_CALL_H

#include "aula_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum aula_call_state {
  AULA_CALL_IDLE = 0,
  AULA_CALL_RESOLVING,
  AULA_CALL_INVITING,
  AULA_CALL_EARLY,
  AULA_CALL_ESTABLISHING_MEDIA,
  AULA_CALL_ESTABLISHED,
  AULA_CALL_TERMINATING,
  AULA_CALL_BACKING_OFF,
  AULA_CALL_FAILED,
  AULA_CALL_TERMINATED,
  AULA_CALL_STOPPED,
  /* A non-retryable or exhausted failure. */
  AULA_CALL_TERMINAL_FAILURE
} aula_call_state;

typedef enum aula_call_event {
  AULA_CALL_EVENT_START = 0,
  AULA_CALL_EVENT_RESOLVED,
  AULA_CALL_EVENT_PROVISIONAL,
  AULA_CALL_EVENT_ACCEPTED,
  /* Media has been opened and is ready to carry the established dialog. */
  AULA_CALL_EVENT_MEDIA_READY,
  AULA_CALL_EVENT_LOCAL_HANGUP,
  AULA_CALL_EVENT_REMOTE_HANGUP,
  AULA_CALL_EVENT_CANCELLED,
  AULA_CALL_EVENT_TIMEOUT,
  AULA_CALL_EVENT_TRANSPORT_ERROR,
  AULA_CALL_EVENT_MEDIA_ERROR,
  AULA_CALL_EVENT_TRANSIENT_FAILURE,
  AULA_CALL_EVENT_PERMANENT_FAILURE,
  AULA_CALL_EVENT_RECONNECT_SCHEDULED,
  AULA_CALL_EVENT_BACKOFF_ELAPSED,
  AULA_CALL_EVENT_TERMINATION_COMPLETE,
  AULA_CALL_EVENT_SHUTDOWN
} aula_call_event;

typedef struct aula_call_transition {
  aula_call_state from;
  aula_call_event event;
  aula_call_state to;
  const char *reason_code;
} aula_call_transition;

typedef struct aula_call_backoff {
  uint32_t attempt;
  uint32_t maximum_attempts;
  uint32_t base_delay_ms;
  uint32_t maximum_delay_ms;
  uint32_t jitter_ms;
} aula_call_backoff;

typedef struct aula_call aula_call;

/* Creates a call with an internally generated opaque correlation identifier. */
aula_status aula_call_create(aula_call **out_call);
/* The returned identifier belongs to call and remains immutable until destroy. */
const char *aula_call_get_correlation_id(const aula_call *call);
aula_call_state aula_call_get_state(const aula_call *call);
aula_status aula_call_apply_event(aula_call *call, aula_call_event event,
                                    aula_call_transition *out_transition);
aula_status aula_call_get_backoff(const aula_call *call,
                                    aula_call_backoff *out_backoff);
/* May be configured only before the call is started. */
aula_status aula_call_set_backoff_policy(aula_call *call,
                                           const aula_call_backoff *policy);
aula_status aula_call_schedule_reconnect(aula_call *call,
                                           uint32_t random_jitter_ms,
                                           uint32_t *out_delay_ms);
/* retry_after_ms is a bounded lower delay request; zero means unspecified. */
aula_status aula_call_schedule_reconnect_with_retry_after(
    aula_call *call, uint32_t random_jitter_ms, uint32_t retry_after_ms,
    uint32_t *out_delay_ms);
void aula_call_destroy(aula_call *call);

#ifdef __cplusplus
}
#endif

#endif
