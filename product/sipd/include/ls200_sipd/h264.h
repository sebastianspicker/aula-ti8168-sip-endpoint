#ifndef LS200_SIPD_H264_H
#define LS200_SIPD_H264_H

#include "ls200_sipd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls200_h264_nal {
  const uint8_t *data;
  size_t length;
  uint8_t type;
  int is_idr;
} ls200_h264_nal;

typedef struct ls200_h264_annexb_iterator {
  ls200_bytes input;
  size_t offset;
  uint32_t maximum_nal_bytes;
} ls200_h264_annexb_iterator;

typedef struct ls200_h264_parameter_sets {
  uint8_t sps[1024];
  size_t sps_length;
  uint8_t pps[1024];
  size_t pps_length;
  uint32_t generation;
  /* RFC 6184 profile-level-id derived from the cached SPS, never an offer
   * default.  It is lowercase hexadecimal and empty until a valid SPS exists. */
  char profile_level_id[7];
  int ready_for_idr;
} ls200_h264_parameter_sets;

typedef struct ls200_h264_packetizer {
  uint16_t mtu;
  uint8_t payload_type;
  uint32_t ssrc;
  uint32_t timestamp;
  uint16_t sequence_number;
} ls200_h264_packetizer;

typedef ls200_status (*ls200_h264_packet_callback)(void *context,
                                                    const ls200_rtp_packet *packet);

typedef struct ls200_h264_depacketizer ls200_h264_depacketizer;

ls200_status ls200_h264_annexb_iterator_init(ls200_h264_annexb_iterator *iterator,
                                             ls200_bytes input,
                                             uint32_t maximum_nal_bytes);
ls200_status ls200_h264_annexb_iterator_next(ls200_h264_annexb_iterator *iterator,
                                             ls200_h264_nal *out_nal);
ls200_status ls200_h264_parameter_sets_update(ls200_h264_parameter_sets *sets,
                                              const ls200_h264_nal *nal);
ls200_status ls200_h264_profile_level_id(const ls200_h264_parameter_sets *sets,
                                         char out_profile_level_id[7]);
int ls200_h264_can_transmit_access_unit(const ls200_h264_parameter_sets *sets,
                                        int access_unit_has_idr);
ls200_status ls200_h264_packetize_single_nal(ls200_h264_packetizer *packetizer,
                                             const ls200_h264_nal *nal,
                                             ls200_h264_packet_callback callback,
                                             void *callback_context);
ls200_status ls200_h264_packetize_fu_a(ls200_h264_packetizer *packetizer,
                                       const ls200_h264_nal *nal,
                                       ls200_h264_packet_callback callback,
                                       void *callback_context);
ls200_status ls200_h264_depacketizer_create(uint32_t maximum_access_unit_bytes,
                                            ls200_h264_depacketizer **out_depacketizer);
ls200_status ls200_h264_depacketizer_push(ls200_h264_depacketizer *depacketizer,
                                          const ls200_rtp_packet *packet,
                                          ls200_mutable_bytes *out_annex_b,
                                          int *out_access_unit_complete);
void ls200_h264_depacketizer_destroy(ls200_h264_depacketizer *depacketizer);

#ifdef __cplusplus
}
#endif

#endif
