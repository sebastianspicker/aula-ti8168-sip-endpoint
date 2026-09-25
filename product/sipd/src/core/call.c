#include "ls200_sipd/call.h"
#include "ls200_sipd/platform.h"

#include <stdlib.h>
#include <string.h>

struct ls200_call {
  char correlation_id[LS200_SIPD_MAX_CORRELATION_ID_BYTES];
  ls200_call_state state;
  ls200_call_backoff backoff;
};

static ls200_status transition(ls200_call *call, ls200_call_event event, ls200_call_state to,
                               const char *reason, ls200_call_transition *out) {
  if (out != NULL) { out->from = call->state; out->event = event; out->to = to; out->reason_code = reason; }
  call->state = to;
  return LS200_STATUS_OK;
}

static ls200_status no_op(const ls200_call *call, ls200_call_event event,
                          const char *reason, ls200_call_transition *out) {
  if (out != NULL) {
    out->from = call->state;
    out->event = event;
    out->to = call->state;
    out->reason_code = reason;
  }
  return LS200_STATUS_OK;
}

static int is_failure_event(ls200_call_event event) {
  return event == LS200_CALL_EVENT_TIMEOUT ||
         event == LS200_CALL_EVENT_TRANSPORT_ERROR ||
         event == LS200_CALL_EVENT_MEDIA_ERROR ||
         event == LS200_CALL_EVENT_TRANSIENT_FAILURE;
}

static const char *failure_reason(ls200_call_event event) {
  if (event == LS200_CALL_EVENT_TIMEOUT) return "timeout";
  if (event == LS200_CALL_EVENT_TRANSPORT_ERROR) return "transport_error";
  if (event == LS200_CALL_EVENT_MEDIA_ERROR) return "media_error";
  return "transient_failure";
}

static int is_active_state(ls200_call_state state) {
  return state >= LS200_CALL_IDLE && state <= LS200_CALL_BACKING_OFF;
}

static int backoff_policy_valid(const ls200_call_backoff *policy) {
  return policy != NULL && policy->base_delay_ms != 0U &&
         policy->maximum_delay_ms >= policy->base_delay_ms &&
         policy->jitter_ms <= policy->maximum_delay_ms;
}

static uint32_t backoff_delay(const ls200_call_backoff *backoff) {
  uint32_t delay = backoff->base_delay_ms;
  uint32_t count = 0U;
  while (count < backoff->attempt && delay < backoff->maximum_delay_ms) {
    if (delay > backoff->maximum_delay_ms / 2U) delay = backoff->maximum_delay_ms;
    else delay *= 2U;
    ++count;
  }
  return delay;
}

ls200_status ls200_call_create(ls200_call **out_call) {
  ls200_call *call;
  uint8_t random_data[16];
  ls200_mutable_bytes random_output;
  static const char hex[] = "0123456789abcdef";
  size_t index;
  if (out_call == NULL || *out_call != NULL) return LS200_STATUS_INVALID_ARGUMENT;
  call = (ls200_call *)calloc(1U, sizeof(*call));
  if (call == NULL) return LS200_STATUS_INTERNAL_ERROR;
  random_output.data = random_data;
  random_output.capacity = sizeof(random_data);
  random_output.length = 0U;
  if (ls200_platform_random_bytes(&random_output) != LS200_STATUS_OK || random_output.length != sizeof(random_data)) { free(call); return LS200_STATUS_IO_ERROR; }
  for (index = 0U; index < sizeof(random_data); ++index) {
    call->correlation_id[index * 2U] = hex[random_data[index] >> 4U];
    call->correlation_id[index * 2U + 1U] = hex[random_data[index] & 0x0fU];
  }
  call->correlation_id[sizeof(random_data) * 2U] = '\0';
  (void)memset(random_data, 0, sizeof(random_data));
  call->state = LS200_CALL_IDLE;
  call->backoff.maximum_attempts = 3U;
  call->backoff.base_delay_ms = 1000U;
  call->backoff.maximum_delay_ms = 30000U;
  call->backoff.jitter_ms = 250U;
  *out_call = call;
  return LS200_STATUS_OK;
}

const char *ls200_call_get_correlation_id(const ls200_call *call) {
  return call == NULL ? NULL : call->correlation_id;
}

ls200_call_state ls200_call_get_state(const ls200_call *call) { return (call == NULL) ? LS200_CALL_STOPPED : call->state; }

static int is_terminal_noop_event(ls200_call_event event) {
  return event == LS200_CALL_EVENT_LOCAL_HANGUP ||
         event == LS200_CALL_EVENT_REMOTE_HANGUP ||
         event == LS200_CALL_EVENT_CANCELLED ||
         event == LS200_CALL_EVENT_TERMINATION_COMPLETE ||
         event == LS200_CALL_EVENT_ACCEPTED ||
         event == LS200_CALL_EVENT_PROVISIONAL;
}

static int is_late_dialog_event(ls200_call_event event) {
  return event == LS200_CALL_EVENT_ACCEPTED ||
         event == LS200_CALL_EVENT_PROVISIONAL ||
         event == LS200_CALL_EVENT_RESOLVED;
}

static int is_terminal_state(ls200_call_state state) {
  return state == LS200_CALL_STOPPED || state == LS200_CALL_TERMINATED ||
         state == LS200_CALL_TERMINAL_FAILURE;
}

static int is_remote_end_event(ls200_call_event event) {
  return event == LS200_CALL_EVENT_REMOTE_HANGUP ||
         event == LS200_CALL_EVENT_CANCELLED;
}

static ls200_status apply_idle_event(ls200_call *call, ls200_call_event event,
                                     ls200_call_transition *out) {
  if (event != LS200_CALL_EVENT_START) return LS200_STATUS_STATE_ERROR;
  return transition(call, event, LS200_CALL_RESOLVING, "start", out);
}

static ls200_status apply_resolving_event(ls200_call *call, ls200_call_event event,
                                          ls200_call_transition *out) {
  if (event != LS200_CALL_EVENT_RESOLVED) return LS200_STATUS_STATE_ERROR;
  return transition(call, event, LS200_CALL_INVITING, "resolved", out);
}

static ls200_status apply_early_event(ls200_call *call, ls200_call_event event,
                                      ls200_call_transition *out) {
  if (event == LS200_CALL_EVENT_PROVISIONAL)
    return no_op(call, event, "duplicate_provisional", out);
  if (event != LS200_CALL_EVENT_ACCEPTED) return LS200_STATUS_STATE_ERROR;
  return transition(call, event, LS200_CALL_ESTABLISHING_MEDIA, "accepted", out);
}

static ls200_status apply_established_event(ls200_call *call, ls200_call_event event,
                                            ls200_call_transition *out) {
  if (!is_late_dialog_event(event) && event != LS200_CALL_EVENT_MEDIA_READY)
    return LS200_STATUS_STATE_ERROR;
  return no_op(call, event, "late_dialog_event", out);
}

static ls200_status apply_failed_event(ls200_call *call, ls200_call_event event,
                                       ls200_call_transition *out) {
  if (event != LS200_CALL_EVENT_RECONNECT_SCHEDULED ||
      call->backoff.attempt > call->backoff.maximum_attempts)
    return LS200_STATUS_STATE_ERROR;
  return transition(call, event, LS200_CALL_BACKING_OFF, "reconnect", out);
}

static ls200_status apply_backoff_event(ls200_call *call, ls200_call_event event,
                                        ls200_call_transition *out) {
  if (event != LS200_CALL_EVENT_BACKOFF_ELAPSED) return LS200_STATUS_STATE_ERROR;
  return transition(call, event, LS200_CALL_RESOLVING, "retry", out);
}

static ls200_status apply_inviting_event(ls200_call *call, ls200_call_event event,
                                         ls200_call_transition *out) {
  if (event == LS200_CALL_EVENT_PROVISIONAL)
    return transition(call, event, LS200_CALL_EARLY, "provisional", out);
  if (event == LS200_CALL_EVENT_ACCEPTED)
    return transition(call, event, LS200_CALL_ESTABLISHING_MEDIA, "accepted", out);
  if (event == LS200_CALL_EVENT_RESOLVED)
    return no_op(call, event, "duplicate_resolved", out);
  return LS200_STATUS_STATE_ERROR;
}

static ls200_status apply_establishing_event(ls200_call *call, ls200_call_event event,
                                             ls200_call_transition *out) {
  if (event == LS200_CALL_EVENT_MEDIA_READY) {
    call->backoff.attempt = 0U;
    return transition(call, event, LS200_CALL_ESTABLISHED, "media_ready", out);
  }
  if (is_late_dialog_event(event)) return no_op(call, event, "late_dialog_event", out);
  return LS200_STATUS_STATE_ERROR;
}

static ls200_status apply_terminating_event(ls200_call *call, ls200_call_event event,
                                            ls200_call_transition *out) {
  if (event == LS200_CALL_EVENT_TERMINATION_COMPLETE)
    return transition(call, event, LS200_CALL_TERMINATED, "terminated", out);
  if (is_late_dialog_event(event) || event == LS200_CALL_EVENT_LOCAL_HANGUP)
    return no_op(call, event, "late_dialog_event", out);
  return LS200_STATUS_STATE_ERROR;
}

static ls200_status apply_state_event(ls200_call *call, ls200_call_event event,
                                      ls200_call_transition *out) {
  switch (call->state) {
    case LS200_CALL_IDLE:
      return apply_idle_event(call, event, out);
    case LS200_CALL_RESOLVING:
      return apply_resolving_event(call, event, out);
    case LS200_CALL_INVITING:
      return apply_inviting_event(call, event, out);
    case LS200_CALL_EARLY:
      return apply_early_event(call, event, out);
    case LS200_CALL_ESTABLISHING_MEDIA:
      return apply_establishing_event(call, event, out);
    case LS200_CALL_ESTABLISHED:
      return apply_established_event(call, event, out);
    case LS200_CALL_TERMINATING:
      return apply_terminating_event(call, event, out);
    case LS200_CALL_FAILED:
      return apply_failed_event(call, event, out);
    case LS200_CALL_BACKING_OFF:
      return apply_backoff_event(call, event, out);
    case LS200_CALL_TERMINATED:
    case LS200_CALL_STOPPED:
    case LS200_CALL_TERMINAL_FAILURE:
      return LS200_STATUS_STATE_ERROR;
  }
  return LS200_STATUS_STATE_ERROR;
}

ls200_status ls200_call_apply_event(ls200_call *call, ls200_call_event event, ls200_call_transition *out_transition) {
  if (call == NULL || event < LS200_CALL_EVENT_START || event > LS200_CALL_EVENT_SHUTDOWN) return LS200_STATUS_INVALID_ARGUMENT;
  if (event == LS200_CALL_EVENT_SHUTDOWN) return transition(call, event, LS200_CALL_STOPPED, "shutdown", out_transition);

  if (is_terminal_state(call->state)) {
    if (is_terminal_noop_event(event)) {
      return no_op(call, event, "terminal_noop", out_transition);
    }
    return LS200_STATUS_STATE_ERROR;
  }

  if (event == LS200_CALL_EVENT_PERMANENT_FAILURE) {
    return transition(call, event, LS200_CALL_TERMINAL_FAILURE, "permanent_failure", out_transition);
  }
  if (is_failure_event(event)) {
    if (call->state == LS200_CALL_FAILED) return no_op(call, event, "duplicate_failure", out_transition);
    return transition(call, event, LS200_CALL_FAILED, failure_reason(event), out_transition);
  }

  if (is_remote_end_event(event)) {
    return transition(call, event, LS200_CALL_TERMINATED, "remote_end", out_transition);
  }
  if (event == LS200_CALL_EVENT_LOCAL_HANGUP) {
    if (call->state == LS200_CALL_BACKING_OFF) {
      return transition(call, event, LS200_CALL_TERMINATED, "cancelled_retry", out_transition);
    }
    if (is_active_state(call->state)) {
      return transition(call, event, LS200_CALL_TERMINATING, "local_end", out_transition);
    }
    if (call->state == LS200_CALL_FAILED) {
      return transition(call, event, LS200_CALL_TERMINATED, "cancelled_retry", out_transition);
    }
  }

  return apply_state_event(call, event, out_transition);
}

ls200_status ls200_call_get_backoff(const ls200_call *call, ls200_call_backoff *out_backoff) {
  if (call == NULL || out_backoff == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  *out_backoff = call->backoff;
  return LS200_STATUS_OK;
}

ls200_status ls200_call_set_backoff_policy(ls200_call *call,
                                           const ls200_call_backoff *policy) {
  if (call == NULL || !backoff_policy_valid(policy)) return LS200_STATUS_INVALID_ARGUMENT;
  if (call->state != LS200_CALL_IDLE) return LS200_STATUS_STATE_ERROR;
  call->backoff = *policy;
  call->backoff.attempt = 0U;
  return LS200_STATUS_OK;
}

ls200_status ls200_call_schedule_reconnect_with_retry_after(
    ls200_call *call, uint32_t random_jitter_ms, uint32_t retry_after_ms,
    uint32_t *out_delay_ms) {
  uint32_t delay;
  ls200_status status;
  if (call == NULL || out_delay_ms == NULL || call->state != LS200_CALL_FAILED) return LS200_STATUS_STATE_ERROR;
  if (call->backoff.attempt >= call->backoff.maximum_attempts) {
    (void)transition(call, LS200_CALL_EVENT_RECONNECT_SCHEDULED,
                     LS200_CALL_TERMINAL_FAILURE, "retry_exhausted", NULL);
    return LS200_STATUS_LIMIT_EXCEEDED;
  }
  delay = backoff_delay(&call->backoff);
  if (retry_after_ms > call->backoff.maximum_delay_ms) retry_after_ms = call->backoff.maximum_delay_ms;
  if (retry_after_ms > delay) delay = retry_after_ms;
  if (random_jitter_ms > call->backoff.jitter_ms) random_jitter_ms = call->backoff.jitter_ms;
  if (delay > call->backoff.maximum_delay_ms - random_jitter_ms) delay = call->backoff.maximum_delay_ms;
  else delay += random_jitter_ms;
  ++call->backoff.attempt;
  status = ls200_call_apply_event(call, LS200_CALL_EVENT_RECONNECT_SCHEDULED, NULL);
  if (status != LS200_STATUS_OK) return status;
  *out_delay_ms = delay;
  return LS200_STATUS_OK;
}

ls200_status ls200_call_schedule_reconnect(ls200_call *call,
                                           uint32_t random_jitter_ms,
                                           uint32_t *out_delay_ms) {
  return ls200_call_schedule_reconnect_with_retry_after(
      call, random_jitter_ms, 0U, out_delay_ms);
}

void ls200_call_destroy(ls200_call *call) { if (call != NULL) { (void)memset(call, 0, sizeof(*call)); free(call); } }
