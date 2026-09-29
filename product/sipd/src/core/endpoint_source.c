#include "endpoint_internal.h"
#include "aula_sipd/platform.h"
#include "../backends/rtsp_private.h"

#include <stdio.h>
#include <string.h>

#define AULA_ENDPOINT_SOURCE_DISCOVERY_MAX_NS UINT64_C(5000000000)

/* Synthetic baseline black-frame parameter sets for the fixture backend. */
static const aula_sdp_codec BASELINE_VIDEO_CODECS[] = {
  {AULA_SDP_CODEC_H264, 96U, 90000U, 1U,
   "packetization-mode=1;profile-level-id=42c01f;sprop-parameter-sets=Z0LAH9kAUAW7ARAAAAMAEAAAAwPA8YMkgA==,aMuMsg=="}
};
const aula_sdp_codec *endpoint_video_codecs(
    const aula_endpoint *endpoint, aula_sdp_codec *dynamic_codec,
    char *dynamic_fmtp, size_t dynamic_fmtp_capacity, size_t *out_count) {
  int written;
  if (out_count == NULL || dynamic_codec == NULL || dynamic_fmtp == NULL)
    return NULL;
  if (endpoint != NULL && endpoint->backend_config.name != NULL &&
      strcmp(endpoint->backend_config.name, "rtsp_native") == 0) {
    if (endpoint->source_h264_profile_present == 0) return NULL;
    written = snprintf(dynamic_fmtp, dynamic_fmtp_capacity,
                       "packetization-mode=1;profile-level-id=%s",
                       endpoint->source_h264_profile_level_id);
    if (written < 0 || (size_t)written >= dynamic_fmtp_capacity) return NULL;
    *dynamic_codec = (aula_sdp_codec){AULA_SDP_CODEC_H264, 96U,
                                       90000U, 1U, dynamic_fmtp};
    *out_count = 1U;
    return dynamic_codec;
  }
  *out_count = sizeof(BASELINE_VIDEO_CODECS) /
               sizeof(BASELINE_VIDEO_CODECS[0]);
  return BASELINE_VIDEO_CODECS;
}

aula_status endpoint_discover_source_h264(aula_endpoint *endpoint) {
  aula_rtsp_h264_capability capability;
  aula_deadline deadline;
  uint64_t budget_ns;
  uint64_t now_ns;
  aula_status status;
  if (endpoint == NULL || endpoint->backend_config.name == NULL)
    return AULA_STATUS_INVALID_ARGUMENT;
  if (strcmp(endpoint->backend_config.name, "rtsp_native") != 0) {
    endpoint->source_h264_profile_present = 0;
    return AULA_STATUS_OK;
  }
  if (aula_platform_monotonic_now(&now_ns) != AULA_STATUS_OK)
    return AULA_STATUS_IO_ERROR;
  budget_ns = (uint64_t)endpoint->view->sip_transaction_timeout_ms *
              UINT64_C(1000000);
  if (budget_ns > AULA_ENDPOINT_SOURCE_DISCOVERY_MAX_NS)
    budget_ns = AULA_ENDPOINT_SOURCE_DISCOVERY_MAX_NS;
  deadline.monotonic_ns = endpoint_deadline_after(now_ns, budget_ns);
  status = aula_rtsp_native_discover_h264(
      &endpoint->backend_config, deadline, &capability);
  if (status != AULA_STATUS_OK) return status;
  (void)memcpy(endpoint->source_h264_profile_level_id,
               capability.profile_level_id,
               sizeof(endpoint->source_h264_profile_level_id));
  endpoint->source_h264_profile_present = 1;
  return AULA_STATUS_OK;
}
