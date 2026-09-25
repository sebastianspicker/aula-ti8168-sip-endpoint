#ifndef LS200_H264_PROFILE_PRIVATE_H
#define LS200_H264_PROFILE_PRIVATE_H

int ls200_h264_profile_level_id_valid(const char profile_level_id[7]);
int ls200_h264_profile_level_id_compatible(const char left[7],
                                           const char right[7]);

#endif
