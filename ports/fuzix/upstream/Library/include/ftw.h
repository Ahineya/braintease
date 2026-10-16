#ifndef _FTW_H
#define	_FTW_H

#include <sys/stat.h>

#define FTW_F   1
#define FTW_D   2
#define FTW_DNR 3
#define FTW_NS  4
#define FTW_SL  5
#define FTW_DP  6
#define FTW_SLN 7	/* No symlinks present */

#define FTW_PHYS  1	/* Does nothing as we have no symlinks */
#define FTW_MOUNT 2
#define FTW_CHDIR 4
#define FTW_DEPTH 8

struct FTW {
	int base;
	int level;
};

int nftw(const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);

#endif
