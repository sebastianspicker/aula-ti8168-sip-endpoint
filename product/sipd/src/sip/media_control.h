#ifndef AULA_SIPD_MEDIA_CONTROL_H
#define AULA_SIPD_MEDIA_CONTROL_H

#include "aula_sipd/common.h"

/* Recognizes only the bounded, unqualified RFC 5168 full-picture command.
 * No general XML, entities, attributes, stream selection or other commands. */
int aula_sip_media_control_is_picture_update(aula_bytes body);

#endif
