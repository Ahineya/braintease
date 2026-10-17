/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SYS_SELECT_H
#define RIPPLE_SYS_SELECT_H
#include <sys/time.h>
#include "../../select-abi.h"
#define FD_SETSIZE 32
typedef struct ripple_fd_set fd_set;
/* Each mask word is 16 bits on both execution targets. */
#define FD_ZERO(s) ((void)((s)->bits[0] = (s)->bits[1] = 0))
#define FD_SET(n,s) ((void)((s)->bits[(unsigned)(n) >> 4] |= (1U << ((unsigned)(n) & 15))))
#define FD_CLR(n,s) ((void)((s)->bits[(unsigned)(n) >> 4] &= ~(1U << ((unsigned)(n) & 15))))
#define FD_ISSET(n,s) (((s)->bits[(unsigned)(n) >> 4] & (1U << ((unsigned)(n) & 15))) != 0)
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#endif
