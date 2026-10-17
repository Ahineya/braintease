/* SPDX-License-Identifier: GPL-2.0-only */
/* FUZIX socket/inode bridge to the shared guest Ethernet/IP/TCP stack.
 * Protocol state lives in reserved physical bank 5, outside the kernel GP and
 * user page pool. Every stack operation and DEVICE transaction runs under di.
 * Packet callbacks only queue kernel data and wake tasks; never copy to users. */
/* Upstream's unused native-net daemon hook has a different net_poll ABI. */
#define net_poll fuzix_native_net_poll
#include "platform.h"
#include <kdata.h>
#include <timer.h>
#include <mmio.h>
#undef net_poll
#include <net_tcp.h>

#define PACKET_SIZE 1500
#define PACKETS 2
#define LISTENERS 2
#define ACCEPT_POOL 4
/* The compact TIME_WAIT helper is shared with the runtime implementation. */
extern int tcp_wait_save(NetTcp *);
struct packet { unsigned length; struct sockaddr_in peer; uint8_t data[PACKET_SIZE]; };
struct endpoint {
    unsigned used, type, protocol, state, head, count, waiting, shut;
    int listener, error;
    inoptr inode;
    struct sockaddr_in local, peer;
    NetTcp *tcp;
    unsigned long closed_at;
    struct packet packets[PACKETS];
};
struct network_state {
    NetContext net;
    NetConfig config;
    NetTcp active[NSOCKET];
    NetTcpListener listeners[LISTENERS];
    NetTcp pools[LISTENERS][ACCEPT_POOL];
    NetTcpWaitTable timewait;
    struct endpoint endpoints[NSOCKET];
    uint8_t scratch[PACKET_SIZE];
    unsigned opened, up, busy;
    unsigned long polled, sequence;
};
#define N ((struct network_state *)0x00050000UL)
typedef char network_state_fits_bank[sizeof(struct network_state) <= 64000UL ? 1 : -1];
static unsigned initialized;

static unsigned swap16(unsigned n) { return (n << 8) | (n >> 8); }
static unsigned read16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static uint32_t packed_ip(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void unpack_ip(uint32_t p,uint8_t *out) {
    unsigned i; for(i=0;i<4;i++) out[i]=(p>>(i*8))&255;
}
static void address(struct sockaddr_in *a,const uint8_t *ip,unsigned port) {
    memset(a,0,sizeof(*a));a->sin_family=AF_INET;
    a->sin_addr.s_addr=packed_ip(ip);a->sin_port=swap16(port);
}
static int error(int e) { udata.u_error=e;return -1; }
static int net_error(int e) {
    if(e==NET_LINK_DOWN)return ENETDOWN;
    if(e==NET_UNREACHABLE||e==NET_PATH_ERROR)return EHOSTUNREACH;
    if(e==NET_TIMEOUT)return ETIMEDOUT;
    if(e==NET_RESET)return ECONNRESET;
    if(e==NET_NO_PORT)return EADDRINUSE;
    if(e==NET_CLOSED||e==NET_EOF)return ENOTCONN;
    if(e==NET_BAD_ARGUMENT)return EINVAL;
    if(e==NET_CONFIG_CHANGED)return ENETDOWN;
    return EIO;
}
static unsigned long now_ms(void) {
    return ticks.full*100UL+ticks_this_dsecond*10UL;
}
static unsigned long sequence(void *unused,const unsigned char *peer,unsigned short port,unsigned long now) {
    used(unused);used(peer);used(port);
    /* The VM RNG is caller-seeded entropy, not a cryptographic entropy source. */
    N->sequence += 64007UL;
    return N->sequence ^ ((uint32_t)rng_get()<<16) ^ rng_get() ^ now;
}
void ripple_network_init(void) {
    irqflags_t flags=di();
    ripple_zero(N,sizeof(*N));
    N->config.address[0]=192;N->config.address[1]=168;N->config.address[2]=64;N->config.address[3]=250;
    N->config.mask[0]=N->config.mask[1]=N->config.mask[2]=255;
    N->config.gateway[0]=192;N->config.gateway[1]=168;N->config.gateway[2]=64;N->config.gateway[3]=1;
    memcpy(N->config.dns,N->config.gateway,4);N->config.seed=rng_get();
    initialized=1;
    irqrestore(flags);
}
static int ensure_network(void) {
    int r;
    if(!initialized)return error(ENETDOWN);
    if(!N->opened) {
        r=net_init(&N->net,&N->config);
        if(r)return error(net_error(r));
        net_tcp_wait_attach(&N->net,&N->timewait);
        net_tcp_enable(&N->net);
        N->opened=1;N->up=1;
        N->polled=now_ms()-10;
    }
    return 0;
}
static void queued(struct endpoint *s,const struct sockaddr_in *peer,const uint8_t *data,unsigned length) {
    struct packet *p;
    if(s->used!=1||(s->shut&1)||s->count==PACKETS||length>PACKET_SIZE)return;
    if(s->state==SS_CONNECTED && (s->peer.sin_addr.s_addr!=peer->sin_addr.s_addr ||
       (s->type==SOCK_DGRAM && s->peer.sin_port!=peer->sin_port)))return;
    p=&s->packets[(s->head+s->count)%PACKETS];
    p->length=length;p->peer=*peer;memcpy(p->data,data,length);++s->count;
    wakeup(s);
}
static void udp_received(NetContext *ctx,const NetDatagram *p,void *user) {
    struct sockaddr_in peer;
    struct endpoint *s=user;
    used(ctx);address(&peer,p->source,p->source_port);queued(s,&peer,p->data,p->length);
}
static void release_endpoint(struct endpoint *s) {
    int r;
    if(s->tcp) {
        if(s->listener>=0) r=net_tcp_release(&N->listeners[s->listener],s->tcp);
        else {
            r=s->tcp->active ? tcp_wait_save(s->tcp) : NET_OK;
            if(!r) memset(s->tcp,0,sizeof(*s->tcp));
        }
        if(r)return;
    }
    memset(s,0,sizeof(*s));
}
void ripple_network_poll(void) {
    struct endpoint *s;
    struct sockaddr_in peer;
    unsigned i,j,length;
    unsigned long now;
    irqflags_t flags;
    int r;
    if(!initialized||!N->opened||!N->up)return;
    flags=di();now=now_ms();
    if(N->busy||now-N->polled<10){irqrestore(flags);return;}
    N->busy=1;N->polled=now;
    r=net_poll(&N->net,now);used(r);
    if(N->net.event==NET_EVENT_ECHO_REPLY||N->net.event==NET_EVENT_ECHO_REQUEST) {
        length=read16(N->net.rx+16);address(&peer,N->net.peer,0);
        if(length<=PACKET_SIZE)for(i=0;i<NSOCKET;i++) {
            s=&N->endpoints[i];if(s->type==SOCK_RAW)queued(s,&peer,N->net.rx+14,length);
        }
    }
    for(i=0;i<LISTENERS;i++)if(N->listeners[i].net)net_tcp_listener_poll(&N->listeners[i],now);
    for(i=0;i<NSOCKET;i++) {
        s=&N->endpoints[i];if(!s->used)continue;
        if(s->tcp && s->listener<0 && s->tcp->active)net_tcp_poll(s->tcp,now);
        if(s->used==2) {
            if(s->tcp && s->tcp->active && now-s->closed_at>=10000UL && s->tcp->state!=NET_TCP_TIME_WAIT)
                net_tcp_abort(s->tcp);
            release_endpoint(s);continue;
        }
        if(s->tcp) {
            if(s->tcp->state==NET_TCP_FAILED)
                s->error=s->state==SS_CONNECTING&&s->tcp->error==NET_RESET?ECONNREFUSED:net_error(s->tcp->error);
            if(s->tcp->state==NET_TCP_ESTABLISHED||s->tcp->state==NET_TCP_CLOSE_WAIT)s->state=SS_CONNECTED;
        }
        if(s->waiting)wakeup(s);
    }
    /* Closed listeners retain their pool until accepted children release it. */
    for(i=0;i<LISTENERS;i++)if(N->listeners[i].net&&!N->listeners[i].active) {
        for(j=0;j<ACCEPT_POOL;j++)if(N->pools[i][j].active||N->pools[i][j].accepted)break;
        if(j==ACCEPT_POOL)memset(&N->listeners[i],0,sizeof(N->listeners[i]));
    }
    N->busy=0;irqrestore(flags);
}

uint_fast8_t issocket(inoptr ino) { return (ino->c_node.i_mode&F_MASK)==F_SOCK; }
static struct endpoint *inode_socket(inoptr ino) {
    unsigned slot=ino->c_node.i_addr[0];
    if(slot>=NSOCKET||N->endpoints[slot].used!=1||N->endpoints[slot].inode!=ino) {
        error(ENOTCONN);return 0;
    }
    return &N->endpoints[slot];
}
static struct endpoint *fd_socket(long fd,unsigned *flags) {
    inoptr ino;
    if(fd<0||fd>=UFTSIZE){error(EBADF);return 0;}
    ino=getinode(fd);if(!ino)return 0;
    if(!issocket(ino)){error(EINVAL);return 0;}
    *flags=of_tab[udata.u_files[fd]].o_access;
    return inode_socket(ino);
}
static int free_slot(void) {
    unsigned i;
    for(i=0;i<NSOCKET;i++)if(!N->endpoints[i].used)return i;
    return error(ENOBUFS);
}
static int make_socket(unsigned slot,unsigned type,unsigned protocol) {
    int fd,oft;
    inoptr ino;
    struct endpoint *s=&N->endpoints[slot];
    if((fd=uf_alloc())<0)return -1;
    if((oft=oft_alloc())<0)return -1;
    /* Sockets need an inode-table entry for descriptor sharing, but must not
     * allocate an on-disk inode: sync/shutdown can run with sockets still open.
     * filesys.c recognizes these NO_DEVICE sockets when syncing/dereferencing. */
    for(ino=i_tab;ino<i_tab+ITABSIZE;ino++)if(!ino->c_refs)break;
    if(ino==i_tab+ITABSIZE){oft_deref(oft);return error(ENFILE);}
    memset(ino,0,sizeof(*ino));ino->c_dev=NO_DEVICE;ino->c_num=slot;
    ino->c_magic=CMAGIC;ino->c_refs=1;
    ino->c_node.i_mode=F_SOCK|0600;ino->c_node.i_addr[0]=slot;
    ino->c_node.i_uid=udata.u_euid;ino->c_node.i_gid=udata.u_egid;
    ino->c_readers=ino->c_writers=1;
    of_tab[oft].o_inode=ino;of_tab[oft].o_access=O_RDWR;of_tab[oft].o_ptr=0;
    udata.u_files[fd]=oft;
    memset(s,0,sizeof(*s));s->used=1;s->type=type;s->protocol=protocol;
    s->state=SS_UNCONNECTED;s->listener=-1;s->inode=ino;
    s->local.sin_family=s->peer.sin_family=AF_INET;
    return fd;
}
static int wait_socket(struct endpoint *s,unsigned flags) {
    int r;
    if(flags&O_NDELAY)return error(EAGAIN);
    ++s->waiting;r=psleep_flags(s,flags);
    /* The scheduler's single-runnable-task fast path returns with IRQs on. */
    di();--s->waiting;
    return r;
}
static int get_address(long ptr,long length,struct sockaddr_in *out) {
    if(length<0||(unsigned long)length<sizeof(*out)||!ptr)return error(EINVAL);
    if(uget((void *)ptr,out,sizeof(*out)))return -1;
    if(out->sin_family!=AF_INET)return error(EAFNOSUPPORT);
    return 0;
}
static int address_room(long ptr,long sizeptr) {
    int n;
    if(!ptr)return 0;
    if(!sizeptr||uget((void *)sizeptr,&n,sizeof(n)))return error(EFAULT);
    if(n<0)return error(EINVAL);
    if(!valaddr_w((void *)sizeptr,sizeof(n),sizeof(n)))return -1;
    if(n>sizeof(struct sockaddr_in))n=sizeof(struct sockaddr_in);
    if(n&&!valaddr_w((void *)ptr,n,n))return -1;
    return 0;
}
static int put_address(long ptr,long sizeptr,const struct sockaddr_in *a) {
    int n,actual=sizeof(*a);
    if(!ptr)return 0;
    if(uget((void *)sizeptr,&n,sizeof(n)))return -1;
    if(n>actual)n=actual;
    if(uput(a,(void *)ptr,n)||uput(&actual,(void *)sizeptr,sizeof(actual)))return -1;
    return 0;
}
static int bound_port(struct endpoint *s,unsigned port) {
    unsigned i;
    for(i=0;i<NSOCKET;i++) {
        struct endpoint *other=&N->endpoints[i];
        if(other!=s&&other->used==1&&other->type==s->type&&other->state!=SS_CONNECTED&&
           other->local.sin_port==swap16(port))return 1;
    }
    return 0;
}
static int autobind(struct endpoint *s) {
    unsigned port,i;
    int r;
    if(s->local.sin_port||s->type==SOCK_RAW)return 0;
    port=49152+(rng_get()&16383);
    for(i=0;i<16384;i++) {if(!bound_port(s,port))break;port=49152+((port+1)&16383);}
    if(i==16384)return error(EADDRINUSE);
    if(s->type==SOCK_DGRAM) {
        r=net_udp_bind(&N->net,port,udp_received,s);
        if(r)return error(net_error(r));
    }
    s->local.sin_port=swap16(port);s->state=SS_BOUND;return 0;
}
static int socket_io(struct endpoint *s,unsigned flags,int writing,struct sockaddr_in *peer) {
    unsigned count=udata.u_count>32767UL?32767:(unsigned)udata.u_count;
    unsigned size,i;
    unsigned short n;
    int r;
    struct packet *p;
    if(!count && s->type==SOCK_STREAM)return 0;
    if(!N->up)return error(ENETDOWN);
    if(writing && (s->shut&2))return error(EPIPE);
    if(!writing && (s->shut&1))return 0;
    for(;;) {
        if(s->error){r=s->error;s->error=0;return error(r);}
        if(s->type==SOCK_STREAM) {
            if(!s->tcp||s->state!=SS_CONNECTED)return error(ENOTCONN);
            size=count>NET_TCP_BUFFER?NET_TCP_BUFFER:count;
            if(writing) {
                if(!valaddr_r(udata.u_base,size,size)||uget(udata.u_base,N->scratch,size))return -1;
                for(i=0;i<size;i++)if(N->scratch[i]>255)return error(EINVAL);
                r=net_tcp_write(s->tcp,N->scratch,size,&n);
                if(!r){udata.u_done=n;return n;}
                if(r!=NET_AGAIN){if(r==NET_CLOSED){ssig(udata.u_ptab,SIGPIPE);return error(EPIPE);}return error(net_error(r));}
            } else {
                if(s->tcp->rx_used) {
                    if(size>s->tcp->rx_used)size=s->tcp->rx_used;
                    if(!valaddr_w(udata.u_base,size,size))return -1;
                    r=net_tcp_read(s->tcp,N->scratch,size,&n);
                    if(!r) {
                        if(uput(N->scratch,udata.u_base,n))return -1;
                        udata.u_done=n;return n;
                    }
                } else if(s->tcp->peer_fin)return 0;
                else if(!s->tcp->active)return error(s->tcp->error?net_error(s->tcp->error):ENOTCONN);
            }
        } else if(writing) {
            if(!peer||!peer->sin_addr.s_addr)return error(EDESTADDRREQ);
            if(count>1472+(s->type==SOCK_RAW?8:0))return error(EMSGSIZE);
            if(count && (!valaddr_r(udata.u_base,count,count)||uget(udata.u_base,N->scratch,count)))return -1;
            for(i=0;i<count;i++)if(N->scratch[i]>255)return error(EINVAL);
            unpack_ip(peer->sin_addr.s_addr,N->scratch+1480);
            if(s->type==SOCK_DGRAM) {
                if(!peer->sin_port)return error(EINVAL);
                if(autobind(s))return -1;
                r=net_udp_send(&N->net,N->scratch+1480,swap16(s->local.sin_port),swap16(peer->sin_port),N->scratch,count);
            } else {
                if(count<8||N->scratch[0]!=8||N->scratch[1])return error(EOPNOTSUPP);
                r=net_icmp_echo(&N->net,N->scratch+1480,read16(N->scratch+4),read16(N->scratch+6),N->scratch+8,count-8);
            }
            if(!r){udata.u_done=count;return count;}
            if(r!=NET_AGAIN)return error(net_error(r));
        } else {
            if(s->type==SOCK_DGRAM&&autobind(s))return -1;
            if(s->count) {
                p=&s->packets[s->head];n=p->length;if(n>count)n=count;
                if(n && (!valaddr_w(udata.u_base,n,n)||uput(p->data,udata.u_base,n)))return -1;
                if(peer)*peer=p->peer;
                s->head=(s->head+1)%PACKETS;--s->count;udata.u_done=n;return n;
            }
            if(!N->net.link)return error(ENETDOWN);
        }
        if(wait_socket(s,flags))return -1;
    }
}
int sock_read(inoptr ino,uint8_t flags) {
    irqflags_t irq=di();struct endpoint *s=inode_socket(ino);int r;
    r=s?socket_io(s,flags,0,0):-1;irqrestore(irq);return r;
}
int sock_write(inoptr ino,uint8_t flags) {
    irqflags_t irq=di();struct endpoint *s=inode_socket(ino);int r;
    r=s?socket_io(s,flags,1,&s->peer):-1;irqrestore(irq);return r;
}
int sock_close(inoptr ino) {
    irqflags_t irq=di();struct endpoint *s=inode_socket(ino);
    if(s) {
        if(s->type==SOCK_DGRAM&&s->local.sin_port)net_udp_unbind(&N->net,swap16(s->local.sin_port));
        if(s->state==SS_LISTENING&&s->listener>=0)net_tcp_unlisten(&N->listeners[s->listener]);
        if(s->tcp) {
            if(net_tcp_shutdown(s->tcp)!=NET_OK && s->tcp->state!=NET_TCP_TIME_WAIT)net_tcp_abort(s->tcp);
            s->used=2;s->inode=0;s->closed_at=now_ms();release_endpoint(s);
        } else memset(s,0,sizeof(*s));
    }
    irqrestore(irq);return 0;
}

arg_t sock_ioctl(inoptr ino,int req,char *data) {
    struct ifreq f;
    struct sockaddr_in *a;
    uint8_t ip[4];
    irqflags_t flags=di();int r=-1;unsigned i;
    if(!inode_socket(ino))goto done;
    if(uget(data,&f,sizeof(f)))goto done;
    if(strcmp(f.ifr_name,"eth0")){error(ENODEV);goto done;}
    if((req&IOCTL_SUPER)&&esuper())goto done;
    a=(void *)&f.ifr_addr;
    switch(req) {
    case SIOCGIFFLAGS:f.ifr_flags=IFF_BROADCAST|(N->up?IFF_UP:0)|(N->net.link?IFF_LINKUP|IFF_RUNNING:0);break;
    case SIOCGIFMTU:f.ifr_mtu=1500;break;
    case SIOCGIFADDR:address(a,N->config.address,0);break;
    case SIOCGIFNETMASK:address(a,N->config.mask,0);break;
    case SIOCGIFGWADDR:address(a,N->config.gateway,0);break;
    case SIOCGIFHWADDR:
        memset(&f.ifr_hwaddr,0,sizeof(f.ifr_hwaddr));f.ifr_hwaddr.sa.family=HW_ETH;
        memcpy(f.ifr_hwaddr.sa.hw.shw_addr,N->net.mac,6);break;
    case SIOCSIFFLAGS:
        if(f.ifr_flags&~(IFF_UP|IFF_BROADCAST|IFF_RUNNING|IFF_LINKUP)){error(EINVAL);goto done;}
        N->up=!!(f.ifr_flags&IFF_UP);
        if(!N->up) {
            net_unconfigure(&N->net);
            for(i=0;i<NSOCKET;i++)if(N->endpoints[i].used==1) {
                if(N->endpoints[i].tcp)net_tcp_abort(N->endpoints[i].tcp);
                N->endpoints[i].error=ENETDOWN;wakeup(&N->endpoints[i]);
            }
            for(i=0;i<LISTENERS;i++)net_tcp_unlisten(&N->listeners[i]);
        } else net_configure(&N->net,&N->config);
        r=0;goto done;
    case SIOCSIFADDR:case SIOCSIFNETMASK:case SIOCSIFGWADDR:
        if(a->sin_family!=AF_INET){error(EAFNOSUPPORT);goto done;}
        unpack_ip(a->sin_addr.s_addr,ip);
        {
            NetConfig candidate=N->config;
            memcpy(req==SIOCSIFADDR?candidate.address:req==SIOCSIFNETMASK?candidate.mask:candidate.gateway,ip,4);
            r=net_configure(&N->net,&candidate);
            if(r){error(net_error(r));r=-1;goto done;}
            N->config=candidate;r=0;goto done;
        }
    default:error(ENOTTY);goto done;
    }
    r=uput(&f,data,sizeof(f));
done:irqrestore(flags);return r;
}

arg_t _netcall(void) {
    long *a=udata.u_net.args;
    struct endpoint *s=0,*child;
    struct sockaddr_in addr;
    unsigned flags=0,i,slot;
    irqflags_t irq;
    int r=-1,fd,op;
    NetTcpConfig cfg;
    NetTcpListenConfig listening;
    NetTcp *accepted;
    if(uget((void *)udata.u_argn,a,sizeof(udata.u_net.args)))return -1;
    if(a[0]<0||a[0]>9)return error(ENOSYS);
    op=a[0];irq=di();
    if(ensure_network())goto done;
    if(op) {s=fd_socket(a[1],&flags);if(!s)goto done;}
    switch(op) {
    case 0:
        if(a[1]!=AF_INET){error(EAFNOSUPPORT);break;}
        if((a[2]!=SOCK_STREAM&&a[2]!=SOCK_DGRAM&&a[2]!=SOCK_RAW)||
           (a[2]==SOCK_STREAM&&a[3]&&a[3]!=IPPROTO_TCP)||
           (a[2]==SOCK_DGRAM&&a[3]&&a[3]!=IPPROTO_UDP)||
           (a[2]==SOCK_RAW&&a[3]!=IPPROTO_ICMP)){error(EPROTONOSUPPORT);break;}
        if(a[2]==SOCK_RAW&&esuper())break;
        fd=free_slot();if(fd<0)break;
        r=make_socket(fd,a[2],a[3]);break;
    case 1:
        if(s->type!=SOCK_STREAM||s->state>SS_BOUND||a[2]<0){error(EINVAL);break;}
        if(autobind(s))break;
        for(i=0;i<LISTENERS;i++)if(!N->listeners[i].net)break;
        if(i==LISTENERS){error(ENOBUFS);break;}
        listening.port=swap16(s->local.sin_port);listening.timeout_ms=600000UL;
        listening.sequence=sequence;listening.user=0;
        r=net_tcp_listen(&N->net,&N->listeners[i],N->pools[i],a[2]<1?1:a[2]>ACCEPT_POOL?ACCEPT_POOL:a[2],&listening);
        if(r){error(net_error(r));r=-1;break;}
        s->state=SS_LISTENING;s->listener=i;r=0;break;
    case 2:
        if(s->state!=SS_UNCONNECTED){error(EINVAL);break;}
        if(get_address(a[2],a[3],&addr))break;
        if(addr.sin_addr.s_addr && addr.sin_addr.s_addr!=packed_ip(N->config.address)){error(EADDRNOTAVAIL);break;}
        if(addr.sin_port&&swap16(addr.sin_port)<1024&&esuper())break;
        if(addr.sin_port&&bound_port(s,swap16(addr.sin_port))){error(EADDRINUSE);break;}
        if(addr.sin_port&&s->type==SOCK_DGRAM) {
            r=net_udp_bind(&N->net,swap16(addr.sin_port),udp_received,s);
            if(r){error(net_error(r));r=-1;break;}
        }
        s->local=addr;s->state=SS_BOUND;r=addr.sin_port?0:autobind(s);break;
    case 3:
        if(s->state==SS_CONNECTED){error(EISCONN);break;}
        if(s->state==SS_LISTENING){error(EINVAL);break;}
        if(s->state==SS_CONNECTING&&s->error){error(s->error);break;}
        if(get_address(a[2],a[3],&addr))break;
        if(!addr.sin_addr.s_addr||(s->type!=SOCK_RAW&&!addr.sin_port)){error(EINVAL);break;}
        if(s->type!=SOCK_STREAM){if(autobind(s))break;s->peer=addr;s->state=SS_CONNECTED;r=0;break;}
        if(s->state!=SS_CONNECTING) {
            if(autobind(s))break;
            slot=s-N->endpoints;s->tcp=&N->active[slot];memset(&cfg,0,sizeof(cfg));
            unpack_ip(addr.sin_addr.s_addr,cfg.peer);cfg.local_port=swap16(s->local.sin_port);
            cfg.remote_port=swap16(addr.sin_port);cfg.timeout_ms=600000UL;
            cfg.initial_sequence=sequence(0,cfg.peer,cfg.remote_port,now_ms());
            r=net_tcp_connect(&N->net,s->tcp,&cfg,now_ms());
            if(r){s->tcp=0;error(net_error(r));r=-1;break;}
            s->peer=addr;address(&s->local,N->config.address,s->tcp->local_port);s->state=SS_CONNECTING;
        } else if(flags&O_NDELAY){r=error(EALREADY);break;}
        if(flags&O_NDELAY){r=error(EINPROGRESS);break;}
        while(s->state==SS_CONNECTING&&!s->error)if(wait_socket(s,flags))goto done;
        r=s->error?error(s->error):0;break;
    case 4:
        if(s->state!=SS_LISTENING){error(EINVAL);break;}
        if(address_room(a[2],a[3]))break;
        for(;;) {
            r=net_tcp_accept(&N->listeners[s->listener],&accepted);
            if(r!=NET_AGAIN)break;
            if(wait_socket(s,flags))goto done;
        }
        if(r){error(net_error(r));r=-1;break;}
        fd=free_slot();if(fd<0){net_tcp_abort(accepted);net_tcp_release(&N->listeners[s->listener],accepted);r=-1;break;}
        r=make_socket(fd,SOCK_STREAM,IPPROTO_TCP);
        if(r<0){net_tcp_abort(accepted);net_tcp_release(&N->listeners[s->listener],accepted);break;}
        child=&N->endpoints[fd];child->tcp=accepted;child->listener=s->listener;child->state=SS_CONNECTED;
        address(&child->local,N->config.address,accepted->local_port);address(&child->peer,accepted->peer,accepted->remote_port);
        if(put_address(a[2],a[3],&child->peer)){int saved=udata.u_error;doclose(r);udata.u_error=saved;r=-1;}
        break;
    case 5:case 9:
        if(op==9&&s->state!=SS_CONNECTED){error(ENOTCONN);break;}
        if(address_room(a[2],a[3]))break;
        r=put_address(a[2],a[3],op==5?&s->local:&s->peer);break;
    case 6:case 7:
        if(a[3]<0||a[4]){error(EINVAL);break;}
        udata.u_base=(void *)a[2];udata.u_count=a[3];udata.u_done=0;
        addr=s->peer;
        if(op==6&&a[5]&&get_address(a[5],a[6],&addr))break;
        if(op==7&&address_room(a[5],a[6]))break;
        r=socket_io(s,flags,op==6,&addr);
        if(r>=0&&op==7&&put_address(a[5],a[6],&addr))r=-1;
        break;
    case 8:
        if(a[2]<0||a[2]>2){error(EINVAL);break;}
        if(s->state!=SS_CONNECTED){error(ENOTCONN);break;}
        if(a[2]!=1)s->shut|=1;
        if(a[2]!=0){s->shut|=2;if(s->tcp)net_tcp_shutdown(s->tcp);}
        r=0;break;
    }
done:irqrestore(irq);return r;
}
