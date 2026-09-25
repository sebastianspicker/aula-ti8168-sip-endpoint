#ifndef LS200_SIPD_CALL_H
#define LS200_SIPD_CALL_H

#include "ls200_sipd/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ls200_call_state {
  LS200_CALL_IDLE = 0,
  LS200_CALL_RESOLVING,
  LS200_CALL_INVITING,
  LS200_CALL_EARLY,
  LS200_CALL_ESTABLISHING_MEDIA,
  LS200_CALL_ESTABLISHED,
  LS200_CALL_TERMINATING,
  LS200_CALL_BACKING_OFF,
  LS200_CALL_FAILED,
  LS200_CALL_TERMINATED,
  LS200_CALL_STOPPED,
  /* A non-retryable or exhausted failure. */
  LS200_CALL_TERMINAL_FAILURE
} ls200_call_state;

typedef enum ls200_call_event {
  LS200_CALL_EVENT_START = 0,
  LS200_CALL_EVENT_RESOLVED,
  LS200_CALL_EVENT_PROVISIONAL,
  LS200_CALL_EVENT_ACCEPTED,
  /* Media has been opened and is ready to carry the established dialog. */
  LS200_CALL_EVENT_MEDIA_READY,
  LS200_CALL_EVENT_LOCAL_HANGUP,
  LS200_CALL_EVENT_REMOTE_HANGUP,
  LS200_CALL_EVENT_CANCELLED,
  LS200_CALL_EVENT_TIMEOUT,
  LS200_CALL_EVENT_TRANSPORT_ERROR,
  LS200_CALL_EVENT_MEDIA_ERROR,
  LS200_CALL_EVENT_TRANSIENT_FAILURE,
  LS200_CALL_EVENT_PERMANENT_FAILURE,
  LS200_CALL_EVENT_RECONNECT_SCHEDULED,
  LS200_CALL_EVENT_BACKOFF_ELAPSED,
  LS200_CALL_EVENT_TERMINATION_COMPLETE,
  LS200_CALL_EVENT_SHUTDOWN
} ls200_call_event;

typedef struct ls200_call_transition {
  ls200_call_state from;
  ls200_call_event event;
  ls200_call_state to;
  const char *reason_code;
} ls200_call_transition;

typedef struct ls200_call_backoff {
  uint32_t attempt;
  uint32_t maximum_attempts;
  uint32_t base_delay_ms;
  uint32_t maximum_delay_ms;
  uint32_t jitter_ms;
} ls200_call_backoff;

typedef struct ls200_call ls200_call;

/* Creates a call with an internally generated opaque correlation identifier. */
ls200_status ls200_call_create(ls200_call **out_call);
/* The returned identifier belongs to call and remains immutable until destroy. */
const char *ls200_call_get_correlation_id(const ls200_call *call);
ls200_call_state ls200_call_get_state(const ls200_call *call);
ls200_status ls200_call_apply_event(ls200_call *call, ls200_call_event event,
                                    ls200_call_transition *out_transition);
ls200_status ls200_call_get_backoff(const ls200_call *call,
                                    ls200_call_backoff *out_backoff);
/* May be configured only before the call is started. */
ls200_status ls200_call_set_backoff_policy(ls200_call *call,
                                           const ls200_call_backoff *policy);
ls200_status ls200_call_schedule_reconnect(ls200_call *call,
                                           uint32_t random_jitter_ms,
                                           uint32_t *out_delay_ms);
/* retry_after_ms is a bounded lower delay request; zero means unspecified. */
ls200_status ls200_call_schedule_reconnect_with_retry_after(
    ls200_call *call, uint32_t random_jitter_ms, uint32_t retry_after_ms,
    uint32_t *out_delay_ms);
void ls200_call_destroy(ls200_call *call);

#ifdef __cplusplus
}
#endif

#endif
