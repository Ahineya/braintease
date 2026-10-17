/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_HOSTFS_H
#define FUSE_X_HOSTFS_H
#include <kernel.h>
#define FUSE_HOST_DEV 0xfffeU
extern char fuse_host_directory[1024];
extern void fuse_host_mount(void);
extern int fuse_host_inode(inoptr);
extern inoptr fuse_host_node(unsigned);
extern inoptr fuse_host_search(inoptr, uint8_t *);
extern inoptr fuse_host_create(inoptr, uint8_t *);
extern bool fuse_host_link(inoptr, uint8_t *, uint8_t *, inoptr);
extern bool fuse_host_empty(inoptr);
extern void fuse_host_read(inoptr);
extern void fuse_host_write(inoptr, unsigned);
extern void fuse_host_deref(inoptr);
extern void fuse_host_store(inoptr);
extern int fuse_host_refresh(inoptr);
extern int fuse_host_open(inoptr, unsigned);
extern int fuse_host_truncate(inoptr, unsigned long);
extern int fuse_host_sync(void);
extern arg_t fuse_host_rename(int *);
extern int fuse_host_chmod(inoptr, unsigned);
#endif
