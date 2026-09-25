#include "endpoint_internal.h"
#include "../media/session_private.h"

#include <stdio.h>
#include <string.h>

ls200_status endpoint_write_metrics(ls200_endpoint *endpoint,
                                    ls200_mutable_bytes *response) {
  ls200_media_metrics metrics;
  uint64_t late_count;
  uint64_t lateness_ms;
  int length;
  if (endpoint == NULL || response == NULL || response->data == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  (void)memset(&metrics, 0, sizeof(metrics));
  if (endpoint->media != NULL)
    (void)ls200_media_session_get_metrics_internal(endpoint->media, &metrics);
#define LS200_METRIC_CLAMP(value) \
    ((value) > UINT64_C(2147483647) ? UINT64_C(2147483647) : (value))
  late_count = LS200_METRIC_CLAMP(endpoint->poll_late_count);
  lateness_ms = LS200_METRIC_CLAMP(
      endpoint->poll_max_lateness_ns / UINT64_C(1000000));
  length = snprintf((char *)response->data, response->capacity,
        "{\"revision\":1,\"reset\":{\"streams\":\"session\",\"poll\":\"process\"},"
        "\"streams\":[{\"kind\":\"audio\",\"queue_depth\":%llu,"
        "\"packet_drops\":%llu,\"access_unit_drops\":0,"
        "\"backend_drops\":%llu,\"backend_restarts\":%llu},"
        "{\"kind\":\"video\",\"queue_depth\":%llu,\"packet_drops\":%llu,"
        "\"access_unit_drops\":%llu,\"backend_drops\":%llu,"
        "\"backend_restarts\":%llu}],\"poll\":{\"late_count\":%llu,"
        "\"max_lateness_ms\":%llu}}",
        (unsigned long long)LS200_METRIC_CLAMP(metrics.audio_queue_depth),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.audio_packet_drops),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.backend_drops),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.backend_restarts),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.video_queue_depth),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.video_packet_drops),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.video_access_unit_drops),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.backend_drops),
        (unsigned long long)LS200_METRIC_CLAMP(metrics.backend_restarts),
      (unsigned long long)late_count, (unsigned long long)lateness_ms);
#undef LS200_METRIC_CLAMP
  if (length < 0 || (size_t)length >= response->capacity)
    return LS200_STATUS_INTERNAL_ERROR;
  response->length = (size_t)length;
  return LS200_STATUS_OK;
}
