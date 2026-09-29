#include <unistd.h>

#define AULA_PJSIP_MEDIA_WATCH_LIMIT 4U

typedef struct aula_pjsip_media_watch {
  int source_descriptor;
  int owned_descriptor;
  pj_ioqueue_key_t *key;
  pj_ioqueue_op_key_t operation;
  unsigned char peek_byte;
  int pending;
  int signaled;
} aula_pjsip_media_watch;

static void pjsip_media_readiness_complete(pj_ioqueue_key_t *key,
                                           pj_ioqueue_op_key_t *operation,
                                           pj_ssize_t bytes_read) {
  aula_pjsip_media_watch *watch =
      (aula_pjsip_media_watch *)pj_ioqueue_get_user_data(key);
  (void)operation;
  if (watch == NULL) return;
  watch->pending = 0;
  if (bytes_read >= 0) watch->signaled = 1;
}

static void pjsip_media_watch_cancel(aula_pjsip_media_watch *watch) {
  if (watch->key != NULL) {
    if (watch->pending != 0)
      (void)pj_ioqueue_clear_key(watch->key);
    /* The pinned Unix/select ioqueue closes the registered socket while
     * unregistering it.  Ownership transferred at successful registration,
     * so closing owned_descriptor again could close a reused descriptor. */
    (void)pj_ioqueue_unregister(watch->key);
    watch->owned_descriptor = -1;
  } else if (watch->owned_descriptor >= 0) {
    (void)close(watch->owned_descriptor);
  }
  (void)memset(watch, 0, sizeof(*watch));
  watch->source_descriptor = -1;
  watch->owned_descriptor = -1;
}

static void pjsip_media_readiness_cancel_instance(
    aula_pjsip_instance *instance) {
  size_t index;
  if (instance == NULL) return;
  for (index = 0U; index < instance->media_watch_count; ++index)
    pjsip_media_watch_cancel(&instance->media_watches[index]);
  instance->media_watch_count = 0U;
}

static int pjsip_media_readiness_matches(const aula_pjsip_instance *instance,
                                         const int *descriptors,
                                         size_t descriptor_count) {
  size_t index;
  if (instance->media_watch_count != descriptor_count) return 0;
  for (index = 0U; index < descriptor_count; ++index) {
    if (instance->media_watches[index].source_descriptor != descriptors[index])
      return 0;
  }
  return 1;
}

static aula_status pjsip_media_watch_register(aula_pjsip_instance *instance,
                                               int descriptor,
                                               aula_pjsip_media_watch *watch) {
  pj_ioqueue_callback callbacks;
  pj_status_t status;
  (void)memset(watch, 0, sizeof(*watch));
  watch->source_descriptor = descriptor;
  watch->owned_descriptor = -1;
  watch->owned_descriptor = dup(descriptor);
  if (watch->owned_descriptor < 0) return AULA_STATUS_IO_ERROR;
  (void)memset(&callbacks, 0, sizeof(callbacks));
  callbacks.on_read_complete = pjsip_media_readiness_complete;
  status = pj_ioqueue_register_sock(
      instance->pool, pjsip_endpt_get_ioqueue(instance->endpoint),
      (pj_sock_t)watch->owned_descriptor, watch, &callbacks, &watch->key);
  if (status != PJ_SUCCESS) {
    (void)close(watch->owned_descriptor);
    watch->owned_descriptor = -1;
    return pjsip_status(status);
  }
  return AULA_STATUS_OK;
}

static aula_status pjsip_media_watch_arm(aula_pjsip_media_watch *watch) {
  pj_ssize_t length = 1;
  pj_status_t status;
  if (watch->pending != 0) return AULA_STATUS_OK;
  watch->signaled = 0;
  pj_ioqueue_op_key_init(&watch->operation, sizeof(watch->operation));
  status = pj_ioqueue_recv(
      watch->key, &watch->operation, &watch->peek_byte, &length,
      PJ_IOQUEUE_ALWAYS_ASYNC | (pj_uint32_t)pj_MSG_PEEK());
  if (status != PJ_EPENDING) return pjsip_status(status);
  watch->pending = 1;
  return AULA_STATUS_OK;
}

static aula_status pjsip_media_readiness_arm(aula_pjsip_instance *instance) {
  size_t index;
  for (index = 0U; index < instance->media_watch_count; ++index) {
    aula_status status = pjsip_media_watch_arm(&instance->media_watches[index]);
    if (status != AULA_STATUS_OK) {
      pjsip_media_readiness_cancel_instance(instance);
      return status;
    }
  }
  return AULA_STATUS_OK;
}

aula_status aula_pjsip_media_readiness_sync(const int *descriptors,
                                               size_t descriptor_count) {
  aula_pjsip_instance *instance = pjsip_active_instance;
  size_t index;
  if (instance == NULL) return AULA_STATUS_UNSUPPORTED;
  if (descriptor_count > AULA_PJSIP_MEDIA_WATCH_LIMIT ||
      (descriptor_count != 0U && descriptors == NULL))
    return AULA_STATUS_INVALID_ARGUMENT;
  if (pjsip_media_readiness_matches(instance, descriptors, descriptor_count))
    return pjsip_media_readiness_arm(instance);
  pjsip_media_readiness_cancel_instance(instance);
  for (index = 0U; index < descriptor_count; ++index) {
    aula_status status = pjsip_media_watch_register(
        instance, descriptors[index], &instance->media_watches[index]);
    if (status != AULA_STATUS_OK) {
      instance->media_watch_count = index + 1U;
      pjsip_media_readiness_cancel_instance(instance);
      return status;
    }
    instance->media_watch_count = index + 1U;
  }
  return pjsip_media_readiness_arm(instance);
}

void aula_pjsip_media_readiness_cancel(void) {
  pjsip_media_readiness_cancel_instance(pjsip_active_instance);
}
