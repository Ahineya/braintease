#ifndef ROS_LOADER_H
#define ROS_LOADER_H

#include "fat16.h"
#include "sys.h"

int sys1_parse(unsigned char *raw, Sys1Header *hdr);
int sys1_load_and_enter(Fat16Fs *fs, Fat16DirEnt *ent);

#endif
