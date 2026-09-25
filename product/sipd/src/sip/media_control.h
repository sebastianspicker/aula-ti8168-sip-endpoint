#ifndef LS200_SIPD_MEDIA_CONTROL_H
#define LS200_SIPD_MEDIA_CONTROL_H

#include "ls200_sipd/common.h"

/* Recognizes only the bounded, unqualified RFC 5168 full-picture command.
 * No general XML, entities, attributes, stream selection or other commands. */
int ls200_sip_media_control_is_picture_update(ls200_bytes body);

#endif
