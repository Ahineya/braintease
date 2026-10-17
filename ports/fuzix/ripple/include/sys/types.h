/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned char uchar_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned long ulong_t;
typedef char *caddr_t;
typedef long daddr_t;
typedef long key_t;
typedef long ssize_t;
typedef unsigned mode_t;
typedef long off_t;
typedef unsigned dev_t;
typedef unsigned gid_t;
typedef unsigned uid_t;
typedef unsigned nlink_t;
typedef int pid_t;
typedef unsigned ino_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned fsfilcnt_t;
/* Current Ripple kernel exports a 32-bit epoch, with high bits reserved. */
typedef unsigned long time_t;
typedef long clock_t;
typedef unsigned long useconds_t;
#define makedev(a, b) (((a) << 8) | (b))
#define major(x) ((x) >> 8)
#define minor(x) ((x) & 255)
#endif
