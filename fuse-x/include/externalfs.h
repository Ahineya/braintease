/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_EXTERNALFS_H
#define FUSE_X_EXTERNALFS_H
#include "hostfs.h"
#include "remotefs-kernel.h"
#define fuse_external_inode(i) (fuse_remote_inode(i)||fuse_host_inode(i))
#define fuse_external_search(i,n) (fuse_remote_inode(i)?fuse_remote_search(i,n):fuse_host_search(i,n))
#define fuse_external_empty(i) (fuse_remote_inode(i)?fuse_remote_empty(i):fuse_host_empty(i))
#define fuse_external_create(i,n) (fuse_remote_inode(i)?fuse_remote_create(i,n):fuse_host_create(i,n))
#define fuse_external_link(i,a,b,n) (fuse_remote_inode(i)?fuse_remote_link(i,a,b,n):fuse_host_link(i,a,b,n))
#define fuse_external_deref(i) (fuse_remote_inode(i)?fuse_remote_deref(i):fuse_host_deref(i))
#define fuse_external_store(i) (fuse_remote_inode(i)?fuse_remote_store(i):fuse_host_store(i))
#define fuse_external_truncate(i,n) (fuse_remote_inode(i)?fuse_remote_truncate(i,n):fuse_host_truncate(i,n))
#define fuse_external_refresh(i) (fuse_remote_inode(i)?fuse_remote_refresh(i):fuse_host_refresh(i))
#define fuse_external_read(i) (fuse_remote_inode(i)?fuse_remote_read(i):fuse_host_read(i))
#define fuse_external_write(i,f) (fuse_remote_inode(i)?fuse_remote_write(i,f):fuse_host_write(i,f))
#define fuse_external_open(i,f) (fuse_remote_inode(i)?fuse_remote_open(i,f):fuse_host_open(i,f))
#define fuse_external_chmod(i,m) (fuse_remote_inode(i)?fuse_remote_chmod(i,m):fuse_host_chmod(i,m))
#define fuse_external_node(n) (dev==FUSE_HOST_DEV?fuse_host_node(n):fuse_remote_node_dev(dev,n))
#define FUSE_EXTERNAL_DEV FUSE_HOST_DEV
static arg_t fuse_external_rename(int *handled) {
    arg_t result=fuse_remote_rename(handled);
    return *handled?result:fuse_host_rename(handled);
}
static int fuse_external_sync(unsigned dev) {
    return dev==FUSE_HOST_DEV?fuse_host_sync():fuse_remote_sync_dev(dev);
}
#endif
