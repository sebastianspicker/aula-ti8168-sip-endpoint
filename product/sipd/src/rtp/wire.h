#ifndef AULA_SIPD_RTP_WIRE_H
#define AULA_SIPD_RTP_WIRE_H

#include "aula_sipd/rtp.h"

aula_status aula_rtp_wire_parse(aula_bytes input, aula_rtp_packet *out_packet);
aula_status aula_rtp_wire_serialize(const aula_rtp_packet *packet,
                                      aula_mutable_bytes *output);

#endif
