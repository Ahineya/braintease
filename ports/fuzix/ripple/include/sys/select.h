/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SYS_SELECT_H
#define RIPPLE_SYS_SELECT_H
#include <sys/time.h>
#include <string.h>
#include "../../select-abi.h"
#define FD_SETSIZE RIPPLE_FILE_LIMIT
typedef struct ripple_fd_set_wide fd_set;
/* Each mask word is 16 bits on both execution targets. */
#define FD_ZERO(s) ((void)memset((s), 0, sizeof(fd_set)))
#define FD_SET(n,s) ((void)((s)->bits[(unsigned)(n) >> 4] |= (1U << ((unsigned)(n) & 15))))
#define FD_CLR(n,s) ((void)((s)->bits[(unsigned)(n) >> 4] &= ~(1U << ((unsigned)(n) & 15))))
#define FD_ISSET(n,s) (((s)->bits[(unsigned)(n) >> 4] & (1U << ((unsigned)(n) & 15))) != 0)
/* A distinct library symbol preserves the two-word select ABI used by older
 * shared-library clients. Rebuilt callers import the expanded entry point. */
#define select ripple_select_wide
int select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
#endif
