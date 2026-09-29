#ifndef AULA_SIPD_H264_H
#define AULA_SIPD_H264_H

#include "aula_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aula_h264_nal {
  const uint8_t *data;
  size_t length;
  uint8_t type;
  int is_idr;
} aula_h264_nal;

typedef struct aula_h264_annexb_iterator {
  aula_bytes input;
  size_t offset;
  uint32_t maximum_nal_bytes;
} aula_h264_annexb_iterator;

typedef struct aula_h264_parameter_sets {
  uint8_t sps[1024];
  size_t sps_length;
  uint8_t pps[1024];
  size_t pps_length;
  uint32_t generation;
  /* RFC 6184 profile-level-id derived from the cached SPS, never an offer
   * default.  It is lowercase hexadecimal and empty until a valid SPS exists. */
  char profile_level_id[7];
  int ready_for_idr;
} aula_h264_parameter_sets;

typedef struct aula_h264_packetizer {
  uint16_t mtu;
  uint8_t payload_type;
  uint32_t ssrc;
  uint32_t timestamp;
  uint16_t sequence_number;
} aula_h264_packetizer;

typedef aula_status (*aula_h264_packet_callback)(void *context,
                                                    const aula_rtp_packet *packet);

typedef struct aula_h264_depacketizer aula_h264_depacketizer;

aula_status aula_h264_annexb_iterator_init(aula_h264_annexb_iterator *iterator,
                                             aula_bytes input,
                                             uint32_t maximum_nal_bytes);
aula_status aula_h264_annexb_iterator_next(aula_h264_annexb_iterator *iterator,
                                             aula_h264_nal *out_nal);
aula_status aula_h264_parameter_sets_update(aula_h264_parameter_sets *sets,
                                              const aula_h264_nal *nal);
aula_status aula_h264_profile_level_id(const aula_h264_parameter_sets *sets,
                                         char out_profile_level_id[7]);
int aula_h264_can_transmit_access_unit(const aula_h264_parameter_sets *sets,
                                        int access_unit_has_idr);
aula_status aula_h264_packetize_single_nal(aula_h264_packetizer *packetizer,
                                             const aula_h264_nal *nal,
                                             aula_h264_packet_callback callback,
                                             void *callback_context);
aula_status aula_h264_packetize_fu_a(aula_h264_packetizer *packetizer,
                                       const aula_h264_nal *nal,
                                       aula_h264_packet_callback callback,
                                       void *callback_context);
aula_status aula_h264_depacketizer_create(uint32_t maximum_access_unit_bytes,
                                            aula_h264_depacketizer **out_depacketizer);
aula_status aula_h264_depacketizer_push(aula_h264_depacketizer *depacketizer,
                                          const aula_rtp_packet *packet,
                                          aula_mutable_bytes *out_annex_b,
                                          int *out_access_unit_complete);
void aula_h264_depacketizer_destroy(aula_h264_depacketizer *depacketizer);

#ifdef __cplusplus
}
#endif

#endif
