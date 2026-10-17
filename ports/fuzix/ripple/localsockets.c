/* SPDX-License-Identifier: GPL-2.0-only */
/* Guest-local, C16 stream transport. No NIC, packet codec or host sockets.
 * Endpoint slots include pending accepts; open-file references own accepted
 * endpoints. Inode references keep bound names stable across unlink/rename. */
#include "platform.h"
#include <kdata.h>
#include "socket-abi.h"
#include "localsockets.h"
#ifdef CONFIG_FUSE_X
#include <native_process.h>
#include <hostfs.h>
#endif

#define LOCAL_SOCKETS 32
#define LOCAL_BUFFER 1024
#define LOCAL_BACKLOG 8
struct local_socket {
    unsigned used, state, shut, head, count, qhead, qcount, backlog;
    uint32_t generation;
    int peer, error;
    inoptr inode, bound;
    struct ripple_sockaddr_un name, peername;
    unsigned pending[LOCAL_BACKLOG];
    uint16_t data[LOCAL_BUFFER];
};
struct local_state {
    uint32_t generation;
    struct local_socket slots[LOCAL_SOCKETS];
};
#ifdef CONFIG_FUSE_X
static struct local_state *L;
#else
#define L ((struct local_state *)0x00070000UL)
#endif
typedef char local_state_fits_bank[sizeof(struct local_state)<=64000UL?1:-1];
static int error(unsigned n) { udata.u_error=n;return -1; }
static struct local_socket *endpoint(inoptr ino) {
    unsigned n=ino->c_node.i_addr[0];
    if(ino->c_dev!=NO_DEVICE||n<NSOCKET||n>=NSOCKET+LOCAL_SOCKETS)return 0;
    n-=NSOCKET;
    return L->slots[n].used&&L->slots[n].inode==ino?&L->slots[n]:0;
}
int ripple_local_inode(inoptr ino) { return endpoint(ino)!=0; }
void ripple_local_init(void) {
#ifdef CONFIG_FUSE_X
    L=rcc_process_kernel_alloc(sizeof(*L));
    if(!L)panic("Fuse-X: cannot allocate local sockets");
    memset(L,0,sizeof(*L));
#else
    ripple_zero(L,sizeof(*L));
#endif
}
static void notify(struct local_socket *s) {
    wakeup(s);
    if(s->inode)selwake_inode(s->inode,SELECT_IN|SELECT_OUT);
}
static int allocate(void) {
    unsigned n;struct local_socket *s;
    for(n=0;n<LOCAL_SOCKETS;n++)if(!L->slots[n].used) {
        s=&L->slots[n];memset(s,0,sizeof(*s));s->used=1;s->state=SS_UNCONNECTED;s->peer=-1;
        if(!++L->generation)++L->generation;
        s->generation=L->generation;
        s->name.family=s->peername.family=AF_UNIX;
        return n;
    }
    return error(ENOBUFS);
}
static void release(struct local_socket *s) {
    struct local_socket *p;
    unsigned n;
    if(s->state==SS_LISTENING) {
        while(s->qcount) {
            n=s->pending[s->qhead];s->qhead=(s->qhead+1)%LOCAL_BACKLOG;--s->qcount;
            if(L->slots[n].peer>=0)L->slots[L->slots[n].peer].error=ECONNRESET;
            release(&L->slots[n]);
        }
    }
    if(s->peer>=0) {
        p=&L->slots[s->peer];p->peer=-1;notify(p);
    }
    notify(s);
    if(s->bound)i_deref(s->bound);
    memset(s,0,sizeof(*s));
}
int ripple_local_close(inoptr ino) {
    struct local_socket *s=endpoint(ino);
    if(s)release(s);
    return 0;
}
static int attach(unsigned n) {
    int fd=ripple_socket_fd(NSOCKET+n);
    if(fd>=0)L->slots[n].inode=getinode(fd);
    return fd;
}
static int wait_local(struct local_socket *s,unsigned flags) {
    int r;
    if(flags&O_NDELAY)return error(EAGAIN);
    r=psleep_flags(s,0);di();return r;
}
unsigned ripple_local_status(inoptr ino) {
    struct local_socket *s=endpoint(ino),*p;unsigned ready=0;
    if(!s)return SELECT_IN|SELECT_OUT;
    if(s->state==SS_LISTENING)return s->qcount?SELECT_IN:0;
    if(s->state!=SS_CONNECTED||s->error)return SELECT_IN|SELECT_OUT;
    p=s->peer<0?0:&L->slots[s->peer];
    if(s->count||(s->shut&1)||!p||(p->shut&2))ready|=SELECT_IN;
    if((s->shut&2)||!p||(p->shut&1)||p->count<LOCAL_BUFFER)ready|=SELECT_OUT;
    return ready;
}
int ripple_local_io(inoptr ino,unsigned flags,int writing) {
    struct local_socket *s=endpoint(ino),*p;
    unsigned n,run,at,count=udata.u_count>32767UL?32767:(unsigned)udata.u_count;
    if(!s)return error(ENOTCONN);
    if(!count)return 0;
    if(s->state!=SS_CONNECTED)return error(ENOTCONN);
    for(;;) {
        p=s->peer<0?0:&L->slots[s->peer];
        if(writing) {
            if((s->shut&2)||!p||(p->shut&1)) {
                ssig(udata.u_ptab,SIGPIPE);return error(EPIPE);
            }
            n=LOCAL_BUFFER-p->count;if(n>count)n=count;
            if(n) {
                if(!valaddr_r(udata.u_base,n,n))return -1;
                at=(p->head+p->count)%LOCAL_BUFFER;
                run=LOCAL_BUFFER-at;if(run>n)run=n;
                if(uget(udata.u_base,p->data+at,run))return -1;
                if(n>run&&uget(udata.u_base+run,p->data,n-run))return -1;
                p->count+=n;notify(p);udata.u_done=n;return n;
            }
        } else {
            if(s->shut&1)return 0;
            n=s->count;if(n>count)n=count;
            if(n) {
                if(!valaddr_w(udata.u_base,n,n))return -1;
                run=LOCAL_BUFFER-s->head;if(run>n)run=n;
                if(uput(s->data+s->head,udata.u_base,run))return -1;
                if(n>run&&uput(s->data,udata.u_base+run,n-run))return -1;
                s->head=(s->head+n)%LOCAL_BUFFER;s->count-=n;
                if(p)notify(p);udata.u_done=n;return n;
            }
            if(s->error){n=s->error;s->error=0;return error(n);}
            if(!p||(p->shut&2))return 0;
        }
        if(wait_local(s,flags))return -1;
    }
}
/* Addresses count C16 cells. Accept full sockaddr_un or the actual terminated
 * path length; unnamed addresses contain only the family cell. */
static int get_name(long ptr,long length,struct ripple_sockaddr_un *a) {
    unsigned n;
    if(length<2||length>sizeof(*a))return error(EINVAL);
    memset(a,0,sizeof(*a));
    if(uget((void *)ptr,a,length))return -1;
    if(a->family!=AF_UNIX)return error(EAFNOSUPPORT);
    if(!a->path[0])return error(EOPNOTSUPP);
    for(n=0;n<(unsigned)length-1;n++)if(!a->path[n])return 0;
    return error(ENAMETOOLONG);
}
static unsigned name_length(struct ripple_sockaddr_un *a) {
    return a->path[0]?strlen(a->path)+2:1;
}
static int name_room(long ptr,long sizeptr,struct ripple_sockaddr_un *a,int copy) {
    int length,actual=name_length(a);unsigned n;
    if(!ptr)return 0;
    if(uget((void *)sizeptr,&length,sizeof(length)))return -1;
    if(length<0)return error(EINVAL);
    n=length<actual?length:actual;
    if(!valaddr_w((void *)sizeptr,sizeof(length),sizeof(length)))return -1;
    if(n&&!valaddr_w((void *)ptr,n,n))return -1;
    if(copy) {
        if(n&&uput(a,(void *)ptr,n))return -1;
        if(uput(&actual,(void *)sizeptr,sizeof(actual)))return -1;
    }
    return 0;
}
static int bind_name(struct local_socket *s,long ptr,long length) {
    struct ripple_sockaddr_un a;
    inoptr ino,parent=0;
    if(s->state!=SS_UNCONNECTED||s->bound)return error(EINVAL);
    if(get_name(ptr,length,&a))return -1;
    ino=n_open(((uint8_t *)ptr)+1,&parent);
    if(ino){i_deref(ino);if(parent)i_deref(parent);return error(EADDRINUSE);}
    if(!parent)return -1;
#ifdef CONFIG_FUSE_X
    /* Host exports do not store guest special nodes. */
    if(fuse_host_inode(parent)){i_deref(parent);return error(EOPNOTSUPP);}
#endif
    ino=newfile(parent,lastname);if(!ino)return -1;
    ino->c_node.i_mode=F_SOCK|(0777&~udata.u_mask);
    setftime(ino,A_TIME|M_TIME|C_TIME);wr_inode(ino);
    s->bound=ino;s->name=a;s->state=SS_BOUND;return 0;
}
static int connect_name(struct local_socket *s,long ptr,long length,unsigned flags) {
    struct ripple_sockaddr_un a;inoptr ino;struct local_socket *listener,*child;
    unsigned i;int n;uint32_t generation;
    if(s->state==SS_CONNECTED)return error(EISCONN);
    if(s->state==SS_LISTENING)return error(EINVAL);
    if(get_name(ptr,length,&a))return -1;
    ino=n_open(((uint8_t *)ptr)+1,0);if(!ino)return -1;
    if(getmode(ino)!=MODE_R(F_SOCK)){i_deref(ino);return error(ECONNREFUSED);}
    if(!(getperm(ino)&OTH_WR)){i_deref(ino);return error(EACCES);}
    for(i=0;i<LOCAL_SOCKETS;i++)if(L->slots[i].used&&L->slots[i].bound==ino)break;
    i_deref(ino);
    if(i==LOCAL_SOCKETS)return error(ECONNREFUSED);
    listener=&L->slots[i];generation=listener->generation;
    for(;;) {
        if(s->state==SS_CONNECTED)return error(EISCONN);
        if(!listener->used||listener->generation!=generation||listener->state!=SS_LISTENING)return error(ECONNREFUSED);
        if(listener->qcount<listener->backlog)break;
        if(wait_local(listener,flags))return -1;
    }
    n=allocate();if(n<0)return -1;
    child=&L->slots[n];child->state=SS_CONNECTED;child->peer=s-L->slots;
    child->name=listener->name;child->peername=s->name;
    s->state=SS_CONNECTED;s->peer=n;s->peername=listener->name;
    listener->pending[(listener->qhead+listener->qcount)%LOCAL_BACKLOG]=n;
    ++listener->qcount;notify(listener);notify(s);return 0;
}
arg_t ripple_local_call(long *a,inoptr ino,unsigned flags) {
    struct local_socket *s=ino?endpoint(ino):0,*child;
    int r=-1,n,fd,pair[2],length,zero=0;
    unsigned op=a[0];
    if(op==0||op==11) {
        if(a[2]!=SOCK_STREAM||a[3])return error(EPROTONOSUPPORT);
        if(op==11&&!valaddr_w((void *)a[4],sizeof(pair),sizeof(pair)))return -1;
        n=allocate();if(n<0)return -1;s=&L->slots[n];
        fd=attach(n);if(fd<0){release(s);return -1;}
        if(op==0)return fd;
        pair[0]=fd;n=allocate();
        if(n<0){doclose(fd);return -1;}
        child=&L->slots[n];fd=attach(n);
        if(fd<0){release(child);doclose(pair[0]);return -1;}
        pair[1]=fd;s->state=child->state=SS_CONNECTED;
        s->peer=n;child->peer=s-L->slots;
        if(uput(pair,(void *)a[4],sizeof(pair))){doclose(pair[0]);doclose(pair[1]);return -1;}
        return 0;
    }
    if(!s)return error(ENOTCONN);
    switch(op) {
    case 1:
        if((s->state!=SS_BOUND&&s->state!=SS_LISTENING)||a[2]<0)return error(EINVAL);
        s->backlog=a[2]<1?1:a[2]>LOCAL_BACKLOG?LOCAL_BACKLOG:a[2];
        s->state=SS_LISTENING;notify(s);return 0;
    case 2:return bind_name(s,a[2],a[3]);
    case 3:return connect_name(s,a[2],a[3],flags);
    case 4:
        if(s->state!=SS_LISTENING)return error(EINVAL);
        /* Validate the caller buffer even before an accept might block. */
        if(name_room(a[2],a[3],&s->name,0))return -1;
        while(!s->qcount)if(wait_local(s,flags))return -1;
        n=s->pending[s->qhead];child=&L->slots[n];
        if(name_room(a[2],a[3],&child->peername,1))return -1;
        fd=attach(n);if(fd<0)return -1;
        s->qhead=(s->qhead+1)%LOCAL_BACKLOG;--s->qcount;notify(s);return fd;
    case 5:case 9:
        if(!a[2])return error(EFAULT);
        if(op==9&&s->state!=SS_CONNECTED)return error(ENOTCONN);
        return name_room(a[2],a[3],op==5?&s->name:&s->peername,1);
    case 6:case 7:
        if(a[3]<0||a[4])return error(EINVAL);
        if(op==6&&a[5])return error(EISCONN);
        if(op==7&&name_room(a[5],a[6],&s->peername,0))return -1;
        udata.u_base=(void *)a[2];udata.u_count=a[3];udata.u_done=0;
        r=ripple_local_io(ino,flags,op==6);
        if(r>=0&&op==7&&name_room(a[5],a[6],&s->peername,1))return -1;
        return r;
    case 8:
        if(a[2]<0||a[2]>2)return error(EINVAL);
        if(s->state!=SS_CONNECTED)return error(ENOTCONN);
        if(a[2]!=1){s->shut|=1;s->count=0;}
        if(a[2]!=0)s->shut|=2;
        notify(s);if(s->peer>=0)notify(&L->slots[s->peer]);return 0;
    case 10:
        if(a[2]!=SOL_SOCKET||a[3]!=SO_ERROR)return error(EOPNOTSUPP);
        if(uget((void *)a[5],&length,sizeof(length)))return -1;
        if(length<0||(unsigned)length<sizeof(zero))return error(EINVAL);
        if(!valaddr_w((void *)a[4],sizeof(zero),sizeof(zero))||!valaddr_w((void *)a[5],sizeof(length),sizeof(length)))return -1;
        zero=s->error;length=sizeof(zero);
        if(uput(&zero,(void *)a[4],sizeof(zero))||uput(&length,(void *)a[5],sizeof(length)))return -1;
        s->error=0;return 0;
    }
    return error(EOPNOTSUPP);
}
