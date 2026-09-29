#include "endpoint_internal.h"
#include "../backends/rtsp_private.h"

#include <arpa/inet.h>
#include <string.h>

enum { ENDPOINT_MAX_SSRCS = 4U, ENDPOINT_REPORT_INTERVAL_MS = 5000U };

static aula_status endpoint_backend(const aula_backend_config *config,
                                     aula_media_backend *out_backend) {
  if (config == NULL || out_backend == NULL || config->name == NULL) {
    return AULA_STATUS_INVALID_ARGUMENT;
  }
  (void)memset(out_backend, 0, sizeof(*out_backend));
  if (strcmp(config->name, "fixture") == 0) {
    return aula_fixture_backend_create(out_backend);
  }
  if (strcmp(config->name, "rtsp_native") == 0 ||
      strcmp(config->name, "rtsp_gst_process") == 0) {
    return aula_rtsp_native_backend_create(out_backend);
  }
  return AULA_STATUS_CONFIGURATION_ERROR;
}

static uint32_t endpoint_queue_bytes(const aula_endpoint *endpoint) {
  uint64_t bytes;
  if (endpoint == NULL || endpoint->view == NULL) return 0U;
  bytes = (uint64_t)(endpoint->view->limits.video_queue_frames +
                     endpoint->view->limits.audio_queue_frames) *
          endpoint->view->limits.rtp_packet_bytes;
  if (bytes == 0U) return 0U;
  if (bytes > (uint64_t)4096U * AULA_SIPD_MAX_RTP_PACKET_BYTES) {
    return 4096U * AULA_SIPD_MAX_RTP_PACKET_BYTES;
  }
  return (uint32_t)bytes;
}

aula_status endpoint_create_media_session(aula_endpoint *endpoint,
                                           aula_media_session **out_media) {
  aula_media_session_config media_config;
  aula_media_backend backend;
  aula_status status;
  if (endpoint == NULL || out_media == NULL || *out_media != NULL ||
      inet_pton(AF_INET, endpoint->addresses.bind_address, endpoint->bind_address) != 1) {
    return AULA_STATUS_CONFIGURATION_ERROR;
  }
  status = endpoint_backend(&endpoint->backend_config, &backend);
  if (status != AULA_STATUS_OK) return status;
  if (endpoint->source_h264_profile_present != 0 &&
      (strcmp(endpoint->backend_config.name, "rtsp_native") == 0 ||
       strcmp(endpoint->backend_config.name, "rtsp_gst_process") == 0)) {
    aula_rtsp_h264_capability capability;
    (void)memset(&capability, 0, sizeof(capability));
    (void)memcpy(capability.profile_level_id,
                 endpoint->source_h264_profile_level_id,
                 sizeof(capability.profile_level_id));
    status = aula_rtsp_native_backend_expect_h264(&backend, &capability);
    if (status != AULA_STATUS_OK) {
      backend.vtable->close(&backend);
      return status;
    }
  }
  (void)memset(&media_config, 0, sizeof(media_config));
  media_config.backend = backend;
  media_config.backend_config = &endpoint->backend_config;
  media_config.transport.local_address = endpoint->bind_address;
  media_config.transport.local_address_length = 4U;
  media_config.transport.minimum_port = endpoint->view->local_rtp_port_min;
  media_config.transport.maximum_port = endpoint->view->local_rtp_port_max;
  media_config.advertised_address = endpoint->addresses.advertised_address;
  media_config.local_cname = "aula-sipd";
  media_config.rtp_mtu = endpoint->view->rtp_mtu;
  media_config.maximum_video_queue_packets = endpoint->view->limits.video_queue_frames;
  media_config.maximum_audio_queue_packets = endpoint->view->limits.audio_queue_frames;
  media_config.maximum_queue_bytes = endpoint_queue_bytes(endpoint);
  media_config.maximum_ssrcs = ENDPOINT_MAX_SSRCS;
  media_config.report_interval_ms = ENDPOINT_REPORT_INTERVAL_MS;
  media_config.aec = (aula_aec_config){endpoint->view->aec_enabled,
                                        endpoint->view->aec_reference_delay_frames,
                                        endpoint->view->aec_delay_calibrated};
  media_config.renderer = endpoint->renderer;
  return aula_media_session_create(&media_config, out_media);
}

void endpoint_handle_picture_fast_update(void *context) {
  aula_endpoint *endpoint = (aula_endpoint *)context;
  if (endpoint == NULL || endpoint->media == NULL ||
      aula_call_get_state(endpoint->call) != AULA_CALL_ESTABLISHED) return;
  /* Acceptance schedules recovery; preserve unsupported-backend accounting. */
  (void)aula_media_session_request_video_keyframe(endpoint->media);
  endpoint_refresh_status(endpoint);
}
