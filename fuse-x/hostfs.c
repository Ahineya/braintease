/* SPDX-License-Identifier: GPL-2.0-only */
/* FUZIX owns names, descriptors, offsets and lifetime. The native service owns
 * only host capabilities and octet conversion; its handles never become FDs. */
#include "host.h"
#include "hostfs.h"
#include <kdata.h>
#include <printf.h>
#include <string.h>

extern long rcc_hosted_fs(unsigned, unsigned long *);
char fuse_host_directory[1024];
static unsigned saved_mode[ITABSIZE];
static unsigned long saved_atime[ITABSIZE], saved_mtime[ITABSIZE];

static long request(unsigned op, unsigned long *a) {
    long result = rcc_hosted_fs(op,a);
    if (result < 0) udata.u_error = -result;
    return result;
}
static unsigned slot(inoptr ino) { return ino-i_tab; }
int fuse_host_inode(inoptr ino) { return ino && ino->c_dev==FUSE_HOST_DEV; }
static int load(inoptr ino) {
    unsigned long info[8], a[8]={0};
    unsigned s=slot(ino);
    a[0]=ino->c_num;a[1]=(unsigned long)info;
    if(request(2,a)<0)return -1;
    ino->c_node.i_mode=info[0];ino->c_node.i_nlink=info[1];
    ino->c_node.i_size=info[2];ino->c_node.i_atime=info[3];
    ino->c_node.i_mtime=info[4];ino->c_node.i_ctime=info[5];
    ino->c_node.i_uid=ino->c_node.i_gid=0;
    saved_mode[s]=info[0];saved_atime[s]=info[3];saved_mtime[s]=info[4];
    return 0;
}
int fuse_host_refresh(inoptr ino) {
    if(ino->c_flags&CDIRTY) fuse_host_store(ino);
    return load(ino);
}
inoptr fuse_host_node(unsigned number) {
    inoptr ino,free=0;
    unsigned long a[8]={0};
    for(ino=i_tab;ino<i_tab+ITABSIZE;ino++) {
        if(ino->c_refs && fuse_host_inode(ino) && ino->c_num==number) {
            if(fuse_host_refresh(ino)<0)return 0;
            return i_ref(ino);
        }
        if(!ino->c_refs)free=ino;
    }
    if(!free) { a[0]=number;request(12,a);udata.u_error=ENFILE;return 0; }
    memset(free,0,sizeof(*free));free->c_dev=FUSE_HOST_DEV;
    free->c_num=number;free->c_super=1;free->c_magic=CMAGIC;
    if(load(free)<0) {a[0]=number;request(12,a);memset(free,0,sizeof(*free));return 0;}
    free->c_refs=1;return free;
}
inoptr fuse_host_search(inoptr parent,uint8_t *name) {
    unsigned long a[8]={0};long number;
    a[0]=parent->c_num;a[1]=(unsigned long)name;
    number=request(1,a);
    if(number<0) {if(udata.u_error==ENOENT)udata.u_error=0;return 0;}
    return fuse_host_node(number);
}
inoptr fuse_host_create(inoptr parent,uint8_t *name) {
    unsigned long a[8]={0};long number;
    inoptr ino=0;
    unsigned mode;
    if(!(getperm(parent)&OTH_WR)) {udata.u_error=EACCES;i_deref(parent);return 0;}
    if(udata.u_callno==51)mode=udata.u_argn1;
    else if(udata.u_callno==4) {
        mode=udata.u_argn1;
        if((mode&F_MASK)!=F_REG) {udata.u_error=EOPNOTSUPP;i_deref(parent);return 0;}
    } else mode=udata.u_argn2;
    a[0]=parent->c_num;a[1]=(unsigned long)name;
    a[2]=mode&~udata.u_mask&0777;a[3]=udata.u_callno==51;
    udata.u_error=0;number=request(5,a);
    if(number>=0)ino=fuse_host_node(number);
    i_deref(parent);return ino;
}
bool fuse_host_link(inoptr parent,uint8_t *old,uint8_t *name,inoptr ino) {
    unsigned long a[8]={0};inoptr target;long result;
    if(!(getperm(parent)&OTH_WR)) {udata.u_error=EACCES;return false;}
    if(ino && (!strcmp((char *)name,".") || !strcmp((char *)name,"..")))return true;
    if(!*name && !ino) {
        target=fuse_host_search(parent,old);
        if(!target) {if(!udata.u_error)udata.u_error=ENOENT;return false;}
        a[0]=parent->c_num;a[1]=(unsigned long)old;a[2]=getmode(target)==MODE_R(F_DIR);
        result=request(6,a);i_deref(target);return result>=0;
    }
    if(*old || !ino || !fuse_host_inode(ino)) {udata.u_error=EXDEV;return false;}
    a[0]=ino->c_num;a[1]=parent->c_num;a[2]=(unsigned long)name;
    return request(7,a)>=0;
}
bool fuse_host_empty(inoptr ino) {
    unsigned long a[8]={0};a[0]=ino->c_num;
    return request(15,a)==1;
}
void fuse_host_read(inoptr ino) {
    unsigned long a[8]={0};long count;
    a[0]=ino->c_num;a[1]=udata.u_offset;
    a[2]=(unsigned long)udata.u_base;a[3]=udata.u_count;
    count=request(3,a);
    if(count<0)udata.u_done=(usize_t)-1;
    else {udata.u_done=0;umove(count);}
}
void fuse_host_write(inoptr ino,unsigned flags) {
    unsigned long a[8]={0};long count;
    a[0]=ino->c_num;a[1]=udata.u_offset;
    a[2]=(unsigned long)udata.u_base;a[3]=udata.u_count;a[4]=!!(flags&O_APPEND);
    count=request(4,a);
    if(count<0)udata.u_done=(usize_t)-1;
    else {udata.u_done=0;umove(count);udata.u_offset=a[5];load(ino);}
}
int fuse_host_open(inoptr ino,unsigned flags) {
    unsigned long a[8]={0};
    unsigned access=O_ACCMODE(flags),permissions=getperm(ino);
    if((access!=O_WRONLY && !(permissions&OTH_RD)) ||
       (access!=O_RDONLY && !(permissions&OTH_WR))) {udata.u_error=EACCES;return -1;}
    a[0]=ino->c_num;a[1]=O_ACCMODE(flags)!=O_RDONLY;
    return request(14,a)<0?-1:0;
}
int fuse_host_truncate(inoptr ino,unsigned long length) {
    unsigned long a[8]={0};
    if(getmode(ino)==MODE_R(F_DIR))return 0; /* Removed directory: no guest blocks. */
    a[0]=ino->c_num;a[1]=length;
    if(request(9,a)<0)return -1;
    return load(ino);
}
int fuse_host_chmod(inoptr ino,unsigned mode) {
    unsigned long a[8]={0};a[0]=ino->c_num;a[1]=mode&0777;
    if(ino->c_node.i_uid!=udata.u_euid && esuper())return -1;
    if(request(10,a)<0)return -1;
    return load(ino);
}
void fuse_host_store(inoptr ino) {
    unsigned long a[8]={0};unsigned s=slot(ino);
    a[0]=ino->c_num;
    if(saved_mode[s]!=ino->c_node.i_mode) {
        a[1]=ino->c_node.i_mode&0777;
        if(request(10,a)<0)return;
        saved_mode[s]=ino->c_node.i_mode;
    }
    if(saved_atime[s]!=ino->c_node.i_atime || saved_mtime[s]!=ino->c_node.i_mtime) {
        a[1]=ino->c_node.i_atime;a[2]=ino->c_node.i_mtime;
        if(request(11,a)<0)return;
        saved_atime[s]=a[1];saved_mtime[s]=a[2];
    }
    ino->c_flags&=~CDIRTY;
}
void fuse_host_deref(inoptr ino) {
    unsigned long a[8]={0};
    if(ino->c_flags&CDIRTY)fuse_host_store(ino);
    if(!--ino->c_refs) {a[0]=ino->c_num;request(12,a);memset(ino,0,sizeof(*ino));}
}
int fuse_host_sync(void) {unsigned long a[8]={0};return request(13,a)<0?-1:0;}

/* Host rename is one atomic host operation, retaining open-file identity. */
arg_t fuse_host_rename(int *handled) {
    inoptr src,sp=0,dst,dp=0;arg_t result=-1;
    uint8_t old[FILENAME_LEN+1];unsigned long a[8]={0};
    src=n_open((void *)udata.u_argn,&sp);
    memcpy(old,lastname,sizeof(old));
    dst=n_open((void *)udata.u_argn1,&dp);
    *handled=fuse_host_inode(src)||fuse_host_inode(sp)||fuse_host_inode(dst)||fuse_host_inode(dp);
    if(*handled) {
        if(!src || !sp || !dp) {if(!udata.u_error)udata.u_error=ENOENT;}
        else if(!fuse_host_inode(src)||!fuse_host_inode(sp)||!fuse_host_inode(dp))udata.u_error=EXDEV;
        else if(src->c_num==ROOTINODE || (dst && dst->c_num==ROOTINODE))udata.u_error=EBUSY;
        else if(!(getperm(sp)&OTH_WR) || !(getperm(dp)&OTH_WR))udata.u_error=EACCES;
        else {
            a[0]=sp->c_num;a[1]=(unsigned long)old;
            a[2]=dp->c_num;a[3]=(unsigned long)lastname;
            udata.u_error=0;result=request(8,a)<0?-1:0;
        }
    }
    if(src)i_deref(src);if(sp)i_deref(sp);if(dst)i_deref(dst);if(dp)i_deref(dp);
    return result;
}
static inoptr mount_directory(inoptr parent,char *name,char *path) {
    inoptr ino=srch_dir(parent,(void *)name);
    unsigned long saved[3];unsigned callno;
    char *scratch=(void *)(PROGBASE+1024);
    if(ino)return ino;
    /* complete_init uses the first 512 cells. This temporary userspace path
     * is validated by the ordinary mkdir syscall and discarded by exec. */
    saved[0]=udata.u_argn;saved[1]=udata.u_argn1;saved[2]=udata.u_argn2;
    callno=udata.u_callno;strcpy(scratch,path);
    udata.u_argn=(unsigned long)scratch;udata.u_argn1=0755;udata.u_callno=51;
    udata.u_error=0;
    if(_mkdir()<0)panic("Fuse-X: cannot create shared-folder mount directory");
    udata.u_argn=saved[0];udata.u_argn1=saved[1];udata.u_argn2=saved[2];udata.u_callno=callno;
    return srch_dir(parent,(void *)name);
}
void fuse_host_mount(void) {
    unsigned long a[8]={0};inoptr parent,point;
    struct mount *m=&fs_tab[1];
    if(!*fuse_host_directory)return;
    if(m->m_dev!=NO_DEVICE)panic("Fuse-X: no shared-folder mount slot");
    a[0]=(unsigned long)fuse_host_directory;
    if(request(0,a)<0) {kprintf("Fuse-X: shared folder error %u\n",udata.u_error);panic("Fuse-X: cannot open shared folder");}
    parent=mount_directory(udata.u_root,"mnt","/mnt");
    if(!parent || getmode(parent)!=MODE_R(F_DIR))panic("Fuse-X: /mnt must be a directory");
    point=mount_directory(parent,"host","/mnt/host");i_deref(parent);
    if(!point || getmode(point)!=MODE_R(F_DIR))panic("Fuse-X: /mnt/host must be a directory");
    memset(m,0,sizeof(*m));m->m_dev=FUSE_HOST_DEV;m->m_mntpt=point;
    m->m_flags=MS_NOSUID|MS_NOATIME;
    m->m_fs.s_mounted=SMOUNTED;m->m_fs.s_fmod=FMOD_CLEAN;
    udata.u_error=0;kputs("Fuse-X: Mac folder mounted read/write at /mnt/host\n");
}
