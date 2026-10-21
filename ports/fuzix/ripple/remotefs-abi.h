/* SPDX-License-Identifier: GPL-2.0-only */
/* Versioned guest userspace-filesystem protocol. No pointers cross processes. */
#ifndef RIPPLE_REMOTEFS_ABI_H
#define RIPPLE_REMOTEFS_ABI_H
#define RFS_SYSCALL 126
#define RFS_VERSION 1
#define RFS_ATTACH 0
#define RFS_NEXT 1
#define RFS_REPLY 2
#define RFS_DETACH 3
#define RFS_STATUS 4
#define RFS_BYTES 4096
#define RFS_MOUNTS 4
struct rfs_status { unsigned version,state,pid;char path[512]; };
struct rfs_message {
    unsigned version, mount, operation;
    unsigned long sequence;
    long result;
    unsigned long arg[8];
    char name[31], name2[31];
    unsigned char data[RFS_BYTES];
};
#endif
