/* SPDX-License-Identifier: GPL-2.0-only */
/* Guest daemon filesystem bridge, shared by hosted and freestanding native.
 * One bounded mailbox per mount. No user pointer survives a context switch. */
#include "remotefs-kernel.h"
#include <kdata.h>
#include <string.h>
extern void fuse_path_save(void *);
extern void fuse_path_restore(void *);
struct remote_mount {
    ptptr daemon, caller;
    unsigned pid, state, failed, mount_index;
    unsigned long serial;
    char path[512];
    struct rfs_message *message;
};
static struct remote_mount mounts[RFS_MOUNTS];
static struct rfs_message *mailboxes[RFS_MOUNTS];
/* Mailbox states: 0 idle, 1 waiting for daemon, 2 being served, 3 reply. */
static int error(unsigned e) { udata.u_error=e;return -1; }
static int daemon_alive(struct remote_mount *m) {
    return m->daemon && m->daemon->p_pid==m->pid && m->daemon->p_status &&
        m->daemon->p_status!=P_ZOMBIE && !m->failed;
}
static int copy_name(char *dest,const char *source) {
    unsigned n;
    for(n=0;n<31;n++){dest[n]=source[n];if(!source[n])return 0;}
    return -1;
}
/* Preserve the pathname walk's entire shared scratch, including kernel-stack
 * pointers, around scheduling. No buffer-cache block is held by these hooks.
 * IOWAIT defers fatal signals until the mailbox and inode resources are safe. */
static void pause_request(struct remote_mount *m) {
    unsigned long scratch[20];
    fuse_path_save(scratch);
    psleep_nosig(m);
    fuse_path_restore(scratch);
}
static void broken(struct remote_mount *m) {
    m->failed=1;wakeup(m);
}
long fuse_remote_request(unsigned dev,unsigned long *a) {
    struct remote_mount *m=&mounts[dev-FUSE_REMOTE_DEV];
    struct rfs_message *q=m->message;
    unsigned op=a[7],i,count=0,old_timeout=udata.u_ptab->p_timeout;
    irqflags_t irq=di();
    long result;
    if(op<1||op>15||op==7){irqrestore(irq);return -EOPNOTSUPP;}
    if(!daemon_alive(m)){irqrestore(irq);return -ENOTCONN;}
    if(m->daemon==udata.u_ptab){irqrestore(irq);return -EBUSY;}
    /* A stopped or wedged service cannot hold a syscall forever. A timeout
     * poisons this attachment; late replies are rejected, never reused. */
    udata.u_ptab->p_timeout=1201;ptimer_insert();
    while(m->state && daemon_alive(m) && udata.u_ptab->p_timeout!=1)pause_request(m);
    if(!daemon_alive(m)||udata.u_ptab->p_timeout==1) {
        if(udata.u_ptab->p_timeout==1)broken(m);
        udata.u_ptab->p_timeout=old_timeout;irqrestore(irq);return -ENOTCONN;
    }
    memset(q,0,sizeof(*q));q->version=RFS_VERSION;q->mount=dev-FUSE_REMOTE_DEV;
    q->operation=op;q->sequence=++m->serial;
    for(i=0;i<8;i++)q->arg[i]=a[i];
    /* Marshal only value fields, names and bounded cell buffers. */
    if(op==1||op==5||op==6||op==8) {
        if(copy_name(q->name,(void *)a[1]))goto long_name;
        q->arg[1]=0;
    }
    if(op==8) {if(copy_name(q->name2,(void *)a[3]))goto long_name;q->arg[3]=0;}
    if(op==2)q->arg[1]=0;
    if(op==3||op==4) {
        count=min(a[3],RFS_BYTES);q->arg[3]=count;q->arg[2]=0;
        if(op==4) {
            if(udata.u_sysio)memcpy(q->data,(void *)a[2],count);
            else if(uget((void *)a[2],q->data,count))goto bad_address;
        } else if(!udata.u_sysio && !valaddr_w((void *)a[2],count,count))goto bad_address;
    }
    m->caller=udata.u_ptab;m->state=1;wakeup(m);
    while(m->state!=3 && daemon_alive(m) && udata.u_ptab->p_timeout!=1)pause_request(m);
    result=-ENOTCONN;
    if(udata.u_ptab->p_timeout==1)broken(m);
    if(m->state==3 && daemon_alive(m)) {
        result=q->result;
        if(result>=0) {
            if(op==2)memcpy((void *)a[1],q->arg,8*sizeof(unsigned long));
            if(op==3) {
                if((unsigned long)result>count)result=-EIO;
                else if(udata.u_sysio)memcpy((void *)a[2],q->data,result);
                else if(result && uput(q->data,(void *)a[2],result))result=-EFAULT;
            }
            if(op==4){if((unsigned long)result>count)result=-EIO;else a[5]=q->arg[5];}
        }
    }
    m->caller=0;m->state=0;wakeup(m);
    udata.u_ptab->p_timeout=old_timeout;irqrestore(irq);return result;
long_name:
    udata.u_ptab->p_timeout=old_timeout;irqrestore(irq);return -ENAMETOOLONG;
bad_address:
    udata.u_ptab->p_timeout=old_timeout;irqrestore(irq);return -EFAULT;
}
void fuse_remote_exit(ptptr process) {
    unsigned i;
    for(i=0;i<RFS_MOUNTS;i++) {
        struct remote_mount *m=&mounts[i];
        if(m->daemon==process) {broken(m);m->daemon=0;}
        if(m->caller==process) {broken(m);m->caller=0;m->state=0;wakeup(m);}
    }
}
static int detach(unsigned index) {
    struct remote_mount *m=&mounts[index];
    struct mount *fs;
    inoptr ino;
    if(!m->mount_index)return error(EINVAL);
    if(m->state)return error(EBUSY);
    for(ino=i_tab;ino<i_tab+ITABSIZE;ino++)
        if(ino->c_refs&&ino->c_dev==FUSE_REMOTE_DEV+index)return error(EBUSY);
    fs=&fs_tab[m->mount_index];
    i_deref(fs->m_mntpt);memset(fs,0,sizeof(*fs));fs->m_dev=NO_DEVICE;
    broken(m);memset(m,0,sizeof(*m));return 0;
}
arg_t fuse_remote_control(void) {
    unsigned op=udata.u_argn,index=udata.u_argn1,i;
    struct remote_mount *m;
    struct rfs_message *user=(void *)udata.u_argn2;
    inoptr point;struct mount *fs;
    irqflags_t irq;
    if(esuper())return -1;
    if(op==RFS_ATTACH) {
        char path[512];unsigned n,component=0;
        usize_t available=valaddr_r((void *)udata.u_argn1,sizeof(path),1);
        if(!available||_uget((void *)udata.u_argn1,path,available))return -1;
        for(n=0;n<available&&path[n];n++);
        if(n==available)return error(ENAMETOOLONG);
        if(n<2||path[0]!='/'||path[n-1]=='/')return error(EINVAL);
        for(n=1;;n++) {
            if(path[n]=='/'||!path[n]) {
                if(!component || (component==1&&path[n-1]=='.') ||
                    (component==2&&path[n-1]=='.'&&path[n-2]=='.'))return error(EINVAL);
                component=0;if(!path[n])break;
            } else {if((unsigned)path[n]>255)return error(EINVAL);component++;}
        }
        point=n_open((void *)udata.u_argn1,0);
        if(!point)return -1;
        if(getmode(point)!=MODE_R(F_DIR)){i_deref(point);return error(ENOTDIR);}
        if(point->c_refs!=1||point->c_num==ROOTINODE||fuse_remote_inode(point)) {
            i_deref(point);return error(EBUSY);
        }
        if(!emptydir(point)){unsigned saved=udata.u_error;i_deref(point);return error(saved?saved:ENOTEMPTY);}
        for(index=0;index<RFS_MOUNTS;index++)if(!mounts[index].mount_index)break;
        for(i=1;i<NMOUNTS;i++)if(fs_tab[i].m_dev==NO_DEVICE)break;
        if(index==RFS_MOUNTS||i==NMOUNTS){i_deref(point);return error(ENOSPC);}
        if(!mailboxes[index])mailboxes[index]=ripple_kernel_alloc(sizeof(struct rfs_message));
        if(!mailboxes[index]){i_deref(point);return error(ENOMEM);}
        m=&mounts[index];memset(m,0,sizeof(*m));m->message=mailboxes[index];m->daemon=udata.u_ptab;
        m->pid=udata.u_ptab->p_pid;m->mount_index=i;
        strcpy(m->path,path);
        fs=&fs_tab[i];memset(fs,0,sizeof(*fs));fs->m_dev=FUSE_REMOTE_DEV+index;
        fs->m_mntpt=point;fs->m_flags=MS_NOSUID|MS_NOATIME;
        fs->m_fs.s_mounted=SMOUNTED;fs->m_fs.s_fmod=FMOD_CLEAN;
        return index;
    }
    if(index>=RFS_MOUNTS)return error(EINVAL);m=&mounts[index];
    if(op==RFS_DETACH)return detach(index);
    if(op==RFS_STATUS) {
        unsigned state=m->mount_index?(daemon_alive(m)?1:2):0;
        if(user){struct rfs_status status;memset(&status,0,sizeof(status));
            status.version=RFS_VERSION;status.state=state;status.pid=m->pid;
            memcpy(status.path,m->path,sizeof(status.path));
            if(uput(&status,user,sizeof(status)))return -1;}
        return state;
    }
    if(m->daemon!=udata.u_ptab||!daemon_alive(m))return error(ENOTCONN);
    if(op==RFS_NEXT) {
        if(!valaddr_w((void *)user,sizeof(*user),sizeof(*user)))return -1;
        irq=di();
        while(daemon_alive(m) && m->state!=1) {
            if(chksigs()){irqrestore(irq);return error(EINTR);}
            psleep(m);
        }
        if(!daemon_alive(m)){irqrestore(irq);return error(ENOTCONN);}
        if(uput(m->message,user,sizeof(*user))){irqrestore(irq);return -1;}
        m->state=2;irqrestore(irq);return 0;
    }
    if(op==RFS_REPLY) {
        /* Read into supervisor memory before validating: the daemon cannot
         * redirect kernel copies or publish a stale response to a new call. */
        struct rfs_message reply;
        if(uget(user,&reply,sizeof(reply)))return -1;
        irq=di();
        if(m->state!=2 || reply.version!=RFS_VERSION || reply.mount!=index ||
           reply.sequence!=m->message->sequence || reply.operation!=m->message->operation ||
           reply.result < -255 || reply.result > 0x7fffffffL) {
            irqrestore(irq);return error(EINVAL);
        }
        /* Only response values and payload are writable. */
        m->message->result=reply.result;
        memcpy(m->message->arg,reply.arg,sizeof(reply.arg));
        memcpy(m->message->data,reply.data,sizeof(reply.data));
        m->state=3;wakeup(m);irqrestore(irq);return 0;
    }
    return error(EINVAL);
}
