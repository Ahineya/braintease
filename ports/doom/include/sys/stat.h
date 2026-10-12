#ifndef DOOM_PORT_SYS_STAT_H
#define DOOM_PORT_SYS_STAT_H
#include <sys/types.h>
struct stat { mode_t st_mode; off_t st_size; };
int mkdir(const char *path, mode_t mode);
int stat(const char *path, struct stat *info);
#endif
