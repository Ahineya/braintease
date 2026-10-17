/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_LOCAL_SOCKETS_H
#define RIPPLE_LOCAL_SOCKETS_H
/* All entry points run under di(), just like the internet socket bridge. */
extern int ripple_socket_fd(unsigned);
extern int ripple_local_inode(inoptr);
extern void ripple_local_init(void);
extern unsigned ripple_local_status(inoptr);
extern int ripple_local_io(inoptr, unsigned, int);
extern int ripple_local_close(inoptr);
extern arg_t ripple_local_call(long *, inoptr, unsigned);
#endif
