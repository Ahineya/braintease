/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RFL frontend boundary: no SSH secrets or protocol data enter the UI. */
#ifdef FUSE_X_USER
#include <errno.h>
#include <string.h>
#include "remotefs.h"
extern long call(unsigned,long,long,long,long);
#endif
int rfl_sftp_status(unsigned index,unsigned long *data,char *path) {
#ifdef FUSE_X_USER
    struct rfs_status st;long result;
    result=call(RFS_SYSCALL,RFS_STATUS,index,(long)&st,0);
    if(result<0)return -errno;
    if(st.version!=RFS_VERSION)return -EIO;
    data[0]=st.state;data[1]=st.pid;memcpy(path,st.path,sizeof(st.path));return 0;
#else
    (void)index;(void)data;(void)path;return -42;
#endif
}
int rfl_sftp_unmount(unsigned index) {
#ifdef FUSE_X_USER
    return call(RFS_SYSCALL,RFS_DETACH,index,0,0)<0?-errno:0;
#else
    (void)index;return -42;
#endif
}
