/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_REMOTEFS_KERNEL_H
#define FUSE_X_REMOTEFS_KERNEL_H
#include <kernel.h>
#include <remotefs-abi.h>
#define FUSE_REMOTE_DEV 0xfff0U
#define fuse_remote_device(d) ((d)>=FUSE_REMOTE_DEV && (d)<FUSE_REMOTE_DEV+4)
extern long fuse_remote_request(unsigned, unsigned long *);
extern inoptr fuse_remote_node_dev(unsigned,unsigned);
extern int fuse_remote_sync_dev(unsigned);
extern arg_t fuse_remote_control(void);
extern void fuse_remote_exit(ptptr);
extern int fuse_remote_inode(inoptr);
extern inoptr fuse_remote_node(unsigned,unsigned);
extern inoptr fuse_remote_search(inoptr, uint8_t *);
extern inoptr fuse_remote_create(inoptr, uint8_t *);
extern bool fuse_remote_link(inoptr, uint8_t *, uint8_t *, inoptr);
extern bool fuse_remote_empty(inoptr);
extern void fuse_remote_read(inoptr);
extern void fuse_remote_write(inoptr, unsigned);
extern void fuse_remote_deref(inoptr);
extern void fuse_remote_store(inoptr);
extern int fuse_remote_refresh(inoptr);
extern int fuse_remote_open(inoptr, unsigned);
extern int fuse_remote_truncate(inoptr, unsigned long);
extern arg_t fuse_remote_rename(int *);
extern int fuse_remote_chmod(inoptr, unsigned);
#endif
