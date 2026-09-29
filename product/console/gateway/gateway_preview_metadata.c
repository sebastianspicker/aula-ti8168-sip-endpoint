#include "gateway_internal.h"

json_t *gateway_preview_status_data(const aula_gateway *gateway) {
  if (gateway->config.preview_enabled) {
    return json_pack("{s:s,s:s,s:s,s:s}", "state", "idle", "transport",
                     "http-flv", "codec", "H.264", "stream_path",
                     "/zoom/api/v1/media/preview.flv");
  }
  return json_pack("{s:s,s:s,s:s,s:s}", "state", "unavailable",
                   "transport", "http-flv", "codec", "H.264", "reason",
                   "not_configured");
}
