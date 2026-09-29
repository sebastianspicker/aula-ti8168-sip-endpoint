#include "adapter_internal.h"
#include "aula_sipd/platform.h"

#define AULA_SIP_PICTURE_FAST_UPDATE_INTERVAL_NS UINT64_C(1000000000)

static int picture_fast_update_dialog_is_live(
    const aula_sip_adapter *adapter, const aula_sip_dialog *target) {
  const aula_sip_dialog *dialog;
  for (dialog = adapter == NULL ? NULL : adapter->dialogs; dialog != NULL;
       dialog = dialog->next) {
    if (dialog == target)
      return dialog->retired == 0 &&
          dialog->accepted_invite_transaction.handle != NULL;
  }
  return 0;
}

void aula_sip_adapter_set_picture_fast_update_callback(
    aula_sip_adapter *adapter,
    aula_sip_picture_fast_update_callback callback, void *context) {
  if (adapter == NULL || callback == NULL ||
      adapter->state == AULA_SIP_ADAPTER_DESTROYED)
    return;
  adapter->picture_fast_update_callback = callback;
  adapter->picture_fast_update_context = context;
}

aula_status aula_sip_adapter_note_picture_fast_update(
    aula_sip_adapter *adapter, void *native_dialog) {
  aula_sip_dialog *dialog;
  uint64_t now_ns;
  if (adapter == NULL || native_dialog == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (adapter->state != AULA_SIP_ADAPTER_ACTIVE ||
      adapter->picture_fast_update_callback == NULL)
    return AULA_STATUS_STATE_ERROR;
  dialog = aula_sip_find_dialog(adapter, native_dialog);
  if (!picture_fast_update_dialog_is_live(adapter, dialog))
    return AULA_STATUS_STATE_ERROR;
  if (adapter->picture_fast_update_pending != 0) {
    adapter->picture_fast_update_observed = 1;
    return AULA_STATUS_OK;
  }
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK)
    return AULA_STATUS_IO_ERROR;
  if (adapter->picture_fast_update_timestamp_valid != 0 &&
      adapter->last_picture_fast_update_dialog == dialog) {
    if (now_ns < adapter->last_picture_fast_update_ns)
      return AULA_STATUS_IO_ERROR;
    if (now_ns - adapter->last_picture_fast_update_ns <
        AULA_SIP_PICTURE_FAST_UPDATE_INTERVAL_NS) {
      adapter->picture_fast_update_observed = 1;
      return AULA_STATUS_OK;
    }
  }
  adapter->last_picture_fast_update_ns = now_ns;
  adapter->last_picture_fast_update_dialog = dialog;
  adapter->picture_fast_update_dialog = dialog;
  adapter->picture_fast_update_timestamp_valid = 1;
  adapter->picture_fast_update_pending = 1;
  adapter->picture_fast_update_observed = 1;
  return AULA_STATUS_OK;
}

aula_status aula_sip_adapter_dispatch_picture_fast_update(
    aula_sip_adapter *adapter, aula_status poll_status) {
  int observed;
  if (adapter == NULL) return AULA_STATUS_INVALID_ARGUMENT;
  if (poll_status != AULA_STATUS_OK && poll_status != AULA_STATUS_AGAIN)
    return poll_status;
  observed = adapter->picture_fast_update_observed;
  adapter->picture_fast_update_observed = 0;
  if (adapter->picture_fast_update_pending != 0) {
    aula_sip_dialog *dialog = adapter->picture_fast_update_dialog;
    aula_sip_picture_fast_update_callback callback =
        adapter->picture_fast_update_callback;
    void *context = adapter->picture_fast_update_context;
    adapter->picture_fast_update_pending = 0;
    adapter->picture_fast_update_dialog = NULL;
    if (callback != NULL && picture_fast_update_dialog_is_live(adapter, dialog))
      callback(context);
  }
  return poll_status == AULA_STATUS_AGAIN && observed != 0 ?
      AULA_STATUS_OK : poll_status;
}

void aula_sip_adapter_forget_picture_fast_update_dialog(
    aula_sip_adapter *adapter, aula_sip_dialog *dialog) {
  if (adapter == NULL || dialog == NULL) return;
  if (adapter->picture_fast_update_dialog == dialog) {
    adapter->picture_fast_update_dialog = NULL;
    adapter->picture_fast_update_pending = 0;
  }
  if (adapter->last_picture_fast_update_dialog == dialog) {
    adapter->last_picture_fast_update_dialog = NULL;
    adapter->last_picture_fast_update_ns = 0U;
    adapter->picture_fast_update_timestamp_valid = 0;
  }
}
