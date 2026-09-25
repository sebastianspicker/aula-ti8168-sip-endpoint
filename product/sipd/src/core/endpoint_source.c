#include "endpoint_internal.h"
#include "ls200_sipd/platform.h"
#include "../backends/rtsp_private.h"

#include <stdio.h>
#include <string.h>

#define LS200_ENDPOINT_SOURCE_DISCOVERY_MAX_NS UINT64_C(5000000000)

static const ls200_sdp_codec BASELINE_VIDEO_CODECS[] = {
  {LS200_SDP_CODEC_H264, 96U, 90000U, 1U,
   "packetization-mode=1;profile-level-id=42e01f;sprop-parameter-sets=Z0LgHtoCgPaEAAADAAQAAAMA8DxYuSg=,aM48gA=="}
};
const ls200_sdp_codec *endpoint_video_codecs(
    const ls200_endpoint *endpoint, ls200_sdp_codec *dynamic_codec,
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
    *dynamic_codec = (ls200_sdp_codec){LS200_SDP_CODEC_H264, 96U,
                                       90000U, 1U, dynamic_fmtp};
    *out_count = 1U;
    return dynamic_codec;
  }
  *out_count = sizeof(BASELINE_VIDEO_CODECS) /
               sizeof(BASELINE_VIDEO_CODECS[0]);
  return BASELINE_VIDEO_CODECS;
}

ls200_status endpoint_discover_source_h264(ls200_endpoint *endpoint) {
  ls200_rtsp_h264_capability capability;
  ls200_deadline deadline;
  uint64_t budget_ns;
  uint64_t now_ns;
  ls200_status status;
  if (endpoint == NULL || endpoint->backend_config.name == NULL)
    return LS200_STATUS_INVALID_ARGUMENT;
  if (strcmp(endpoint->backend_config.name, "rtsp_native") != 0) {
    endpoint->source_h264_profile_present = 0;
    return LS200_STATUS_OK;
  }
  if (ls200_platform_monotonic_now(&now_ns) != LS200_STATUS_OK)
    return LS200_STATUS_IO_ERROR;
  budget_ns = (uint64_t)endpoint->view->sip_transaction_timeout_ms *
              UINT64_C(1000000);
  if (budget_ns > LS200_ENDPOINT_SOURCE_DISCOVERY_MAX_NS)
    budget_ns = LS200_ENDPOINT_SOURCE_DISCOVERY_MAX_NS;
  deadline.monotonic_ns = endpoint_deadline_after(now_ns, budget_ns);
  status = ls200_rtsp_native_discover_h264(
      &endpoint->backend_config, deadline, &capability);
  if (status != LS200_STATUS_OK) return status;
  (void)memcpy(endpoint->source_h264_profile_level_id,
               capability.profile_level_id,
               sizeof(endpoint->source_h264_profile_level_id));
  endpoint->source_h264_profile_present = 1;
  return LS200_STATUS_OK;
}

