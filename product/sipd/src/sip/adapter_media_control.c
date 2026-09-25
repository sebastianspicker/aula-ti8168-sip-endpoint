#include "adapter_internal.h"
#include "ls200_sipd/platform.h"

#define LS200_SIP_PICTURE_FAST_UPDATE_INTERVAL_NS UINT64_C(1000000000)

static int picture_fast_update_dialog_is_live(
    const ls200_sip_adapter *adapter, const ls200_sip_dialog *target) {
  const ls200_sip_dialog *dialog;
  for (dialog = adapter == NULL ? NULL : adapter->dialogs; dialog != NULL;
       dialog = dialog->next) {
    if (dialog == target)
      return dialog->retired == 0 &&
          dialog->accepted_invite_transaction.handle != NULL;
  }
  return 0;
}

void ls200_sip_adapter_set_picture_fast_update_callback(
    ls200_sip_adapter *adapter,
    ls200_sip_picture_fast_update_callback callback, void *context) {
  if (adapter == NULL || callback == NULL ||
      adapter->state == LS200_SIP_ADAPTER_DESTROYED)
    return;
  adapter->picture_fast_update_callback = callback;
  adapter->picture_fast_update_context = context;
}

ls200_status ls200_sip_adapter_note_picture_fast_update(
    ls200_sip_adapter *adapter, void *native_dialog) {
  ls200_sip_dialog *dialog;
  uint64_t now_ns;
  if (adapter == NULL || native_dialog == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (adapter->state != LS200_SIP_ADAPTER_ACTIVE ||
      adapter->picture_fast_update_callback == NULL)
    return LS200_STATUS_STATE_ERROR;
  dialog = ls200_sip_find_dialog(adapter, native_dialog);
  if (!picture_fast_update_dialog_is_live(adapter, dialog))
    return LS200_STATUS_STATE_ERROR;
  if (adapter->picture_fast_update_pending != 0) {
    adapter->picture_fast_update_observed = 1;
    return LS200_STATUS_OK;
  }
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK)
    return LS200_STATUS_IO_ERROR;
  if (adapter->picture_fast_update_timestamp_valid != 0 &&
      adapter->last_picture_fast_update_dialog == dialog) {
    if (now_ns < adapter->last_picture_fast_update_ns)
      return LS200_STATUS_IO_ERROR;
    if (now_ns - adapter->last_picture_fast_update_ns <
        LS200_SIP_PICTURE_FAST_UPDATE_INTERVAL_NS) {
      adapter->picture_fast_update_observed = 1;
      return LS200_STATUS_OK;
    }
  }
  adapter->last_picture_fast_update_ns = now_ns;
  adapter->last_picture_fast_update_dialog = dialog;
  adapter->picture_fast_update_dialog = dialog;
  adapter->picture_fast_update_timestamp_valid = 1;
  adapter->picture_fast_update_pending = 1;
  adapter->picture_fast_update_observed = 1;
  return LS200_STATUS_OK;
}

ls200_status ls200_sip_adapter_dispatch_picture_fast_update(
    ls200_sip_adapter *adapter, ls200_status poll_status) {
  int observed;
  if (adapter == NULL) return LS200_STATUS_INVALID_ARGUMENT;
  if (poll_status != LS200_STATUS_OK && poll_status != LS200_STATUS_AGAIN)
    return poll_status;
  observed = adapter->picture_fast_update_observed;
  adapter->picture_fast_update_observed = 0;
  if (adapter->picture_fast_update_pending != 0) {
    ls200_sip_dialog *dialog = adapter->picture_fast_update_dialog;
    ls200_sip_picture_fast_update_callback callback =
        adapter->picture_fast_update_callback;
    void *context = adapter->picture_fast_update_context;
    adapter->picture_fast_update_pending = 0;
    adapter->picture_fast_update_dialog = NULL;
    if (callback != NULL && picture_fast_update_dialog_is_live(adapter, dialog))
      callback(context);
  }
  return poll_status == LS200_STATUS_AGAIN && observed != 0 ?
      LS200_STATUS_OK : poll_status;
}

void ls200_sip_adapter_forget_picture_fast_update_dialog(
    ls200_sip_adapter *adapter, ls200_sip_dialog *dialog) {
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
