#ifndef LS200_SIPD_RTP_WIRE_H
#define LS200_SIPD_RTP_WIRE_H

#include "ls200_sipd/rtp.h"

ls200_status ls200_rtp_wire_parse(ls200_bytes input, ls200_rtp_packet *out_packet);
ls200_status ls200_rtp_wire_serialize(const ls200_rtp_packet *packet,
                                      ls200_mutable_bytes *output);

#endif
