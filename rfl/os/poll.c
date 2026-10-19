/* Shared asynchronous guest engine. Each call makes bounded nonblocking progress. */
#include "socket.h"

unsigned long rfl_io_remaining(unsigned long now,unsigned long end) {
    return (long)(now-end)>=0?0:end-now;
}
int rfl_poll_clock(unsigned long *out) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return -1;
    *out=(unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;return 0;
}
void *rfl_poll_create(void) {
    RflPoll *p=calloc(1,sizeof(*p));if(p)p->refs=1;else errno=ENOMEM;return p;
}
void *rfl_poll_retain(void *pointer) {
    RflPoll *p=pointer;if(p->refs==0xffffffffUL){errno=EINVAL;return NULL;}++p->refs;return p;
}
void rfl_poll_release(void *pointer) {RflPoll *p=pointer;if(!--p->refs)free(p);}
void *rfl_poll_driver(void *pointer) {
    RflPoll *p=pointer;
    if(p->driver){errno=EBUSY;return NULL;}
    if(!rfl_poll_retain(p))return NULL;
    p->driver=1;return p;
}
void rfl_poll_driver_drop(void *pointer) {RflPoll *p=pointer;p->driver=0;rfl_poll_release(p);}
RflWait *rfl_io_create(RflPoll *p,RflSocket *socket,unsigned events,unsigned kind) {
    RflWait *w;unsigned i;
    if(!events||(events&~3U)){errno=EINVAL;return NULL;}
    for(i=0;i<RFL_IO_SLOTS&&p->slots[i];++i){}
    if(i==RFL_IO_SLOTS){errno=ENOBUFS;return NULL;}
    w=calloc(1,sizeof(*w));if(!w){errno=ENOMEM;return NULL;}
    w->poll=rfl_poll_retain(p);
    if(socket)w->socket=rfl_socket_retain(socket);
    if(!w->poll||(socket&&!w->socket)){
        if(w->poll)rfl_poll_release(w->poll);
        if(w->socket)rfl_socket_release(w->socket);
        free(w);return NULL;
    }
    w->slot=i;w->events=events;w->kind=kind;w->active=1;p->slots[i]=w;return w;
}
void *rfl_poll_watch(void *p,void *socket,unsigned events) {
    return rfl_io_create(p,socket,events,RFL_IO_WAIT);
}
void rfl_io_finish(RflWait *w,int status,int error) {
    if(w->status)return;
    w->status=status;w->error=error;
    if(w->kind!=RFL_IO_WAIT){
        if(w->socket){
            if(w->kind==RFL_IO_READ)w->socket->reading=0;
            if(w->kind==RFL_IO_WRITE)w->socket->writing=0;
            rfl_socket_release(w->socket);w->socket=NULL;
        }
        if(w->kind==RFL_IO_WRITE){free(w->buffer);w->buffer=NULL;}
        if(w->dns)rfl_dns_close(w);
    }
}
unsigned rfl_poll_slot(void *p) {return ((RflWait *)p)->slot;}
int rfl_poll_error(void *p) {return ((RflWait *)p)->error;}
int rfl_poll_timeout(void *p,unsigned long milliseconds) {
    RflWait *w=p;
    if(milliseconds>0x7fffffffUL||w->started){errno=EINVAL;return -1;}
    w->has_timeout=1;w->duration=milliseconds;return 0;
}
static void step(RflWait *w,unsigned long now) {
    long n;unsigned long count;unsigned budget;int error; socklen_t length;
    if(!w->active||!w->started||w->status)return;
    if(w->has_timeout&&!rfl_io_remaining(now,w->deadline)){
        rfl_io_finish(w,RFL_IO_TIMED_OUT,ETIMEDOUT);return;
    }
    if(w->kind==RFL_IO_DNS){rfl_dns_step(w,now);return;}
    if(w->kind==RFL_IO_WAIT)return;
    if(w->kind==RFL_IO_CONNECT){
        if(!w->offset){
            n=connect(w->socket->fd,(void *)&w->peer,sizeof(w->peer));w->offset=1;
            if(n<0&&errno!=EINPROGRESS&&errno!=EALREADY&&errno!=EAGAIN&&errno!=EINTR&&errno!=EISCONN){
                rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;
            }
            if(n<0&&errno!=EISCONN)return;
        }else{
            error=0;length=sizeof(error);
            if(getsockopt(w->socket->fd,SOL_SOCKET,SO_ERROR,&error,&length)<0){rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;}
            if(error){rfl_io_finish(w,RFL_IO_OS_ERROR,error);return;}
            length=sizeof(w->peer);
            if(getpeername(w->socket->fd,(void *)&w->peer,&length)<0){
                if(errno==ENOTCONN||errno==EINPROGRESS)return;
                rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;
            }
        }
        w->result_socket=w->socket;w->socket=NULL;rfl_io_finish(w,RFL_IO_READY,0);return;
    }
    if(w->kind==RFL_IO_READ){
        if(!w->size){rfl_io_finish(w,RFL_IO_READY,0);return;}
        n=recv(w->socket->fd,w->buffer,w->size,0);
        if(n>=0){w->offset=n;rfl_io_finish(w,RFL_IO_READY,0);}
        else if(errno!=EAGAIN&&errno!=EINTR)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);
        return;
    }
    for(budget=0;budget<4&&w->offset<w->size;++budget){
        count=w->size-w->offset;if(count>32767UL)count=32767UL;
        n=send(w->socket->fd,w->buffer+w->offset,count,0);
        if(n>0){w->offset+=n;continue;}
        if(n<0&&(errno==EAGAIN||errno==EINTR))return;
        rfl_io_finish(w,RFL_IO_OS_ERROR,n<0?errno:EIO);return;
    }
    if(w->offset==w->size)rfl_io_finish(w,RFL_IO_READY,0);
}
int rfl_poll_status(void *p,unsigned long now) {
    RflWait *w=p;
    if(!w->active)return w->status?w->status:RFL_IO_CANCELLED;
    if(!w->started){w->started=1;w->deadline=now+w->duration;step(w,now);}
    return w->status;
}
void rfl_poll_cancel(void *p) {
    RflWait *w=p;
    if(w->active){
        w->poll->slots[w->slot]=NULL;w->active=0;
        if(!w->status)rfl_io_finish(w,RFL_IO_CANCELLED,0);
        if(w->socket){rfl_socket_release(w->socket);w->socket=NULL;}
    }
}
void rfl_poll_drop(void *p) {
    RflWait *w=p;rfl_poll_cancel(w);
    if(w->result_socket)rfl_socket_release(w->result_socket);
    free(w->buffer);if(w->dns)rfl_dns_destroy(w);
    rfl_poll_release(w->poll);free(w);
}
static int wait_fd(RflWait *w) {return w->dns?rfl_dns_fd(w):w->socket?w->socket->fd:-1;}
static unsigned wait_events(RflWait *w) {return w->dns?rfl_dns_events(w):w->events;}
/* events[0] is count; remaining cells are ready slot indices. At most 32 wakes
 * cross the C/RFL boundary per turn. Clock accounting and deadline scans stay C. */
int rfl_poll_turn(void *pointer,unsigned long hint,unsigned long *sample,unsigned short *events) {
    RflPoll *p=pointer;RflWait *w;fd_set reads,writes;struct timeval timeout;
    unsigned i,interest;int fd,nfds=0,n;unsigned long now,elapsed,delay=100,left;
    events[0]=0;
    if(!p->driver){errno=EINVAL;return -1;}
    if(rfl_poll_clock(&now)<0)return -1;
    elapsed=now-*sample;if(elapsed>0x7fffffffUL){errno=EINVAL;return -1;}
    if(hint!=0xffffffffUL){
        if(hint>0x7fffffffUL){errno=EINVAL;return -1;}
        left=elapsed>=hint?0:hint-elapsed;if(left<delay)delay=left;
    }
    FD_ZERO(&reads);FD_ZERO(&writes);
    for(i=0;i<RFL_IO_SLOTS;++i)if((w=p->slots[i])&&w->started){
        step(w,now);
        if(w->status){if(!w->notified)delay=0;continue;}
        if(w->has_timeout){left=rfl_io_remaining(now,w->deadline);if(left<delay)delay=left;}
        if(w->dns)delay=rfl_dns_delay(w,now,delay);
        fd=wait_fd(w);if(fd<0)continue;
        if(fd>=FD_SETSIZE){errno=EINVAL;return -1;}
        interest=wait_events(w);
        if(interest&1)FD_SET(fd,&reads);
        if(interest&2)FD_SET(fd,&writes);
        if(fd>=nfds)nfds=fd+1;
    }
    /* Processing registrations is part of the caller's deadline, not extra idle. */
    {unsigned long after;if(rfl_poll_clock(&after)<0)return -1;
     elapsed=after-now;if(elapsed>0x7fffffffUL){errno=EINVAL;return -1;}
     delay=elapsed>=delay?0:delay-elapsed;now=after;}
    timeout.tv_sec=delay/1000;timeout.tv_usec=(delay%1000)*1000;
    n=select(nfds,&reads,&writes,NULL,&timeout);
    if(n<0&&errno!=EINTR)return -1;
    {unsigned long after;if(rfl_poll_clock(&after)<0)return -1;
     if(after-now>0x7fffffffUL){errno=EINVAL;return -1;}now=after;}
    for(i=0;i<RFL_IO_SLOTS;++i)if((w=p->slots[i])&&w->started){
        if(!w->status){
            fd=wait_fd(w);interest=wait_events(w);
            if(w->kind==RFL_IO_WAIT){
                if(n>=0&&fd>=0&&(((interest&1)&&FD_ISSET(fd,&reads))||((interest&2)&&FD_ISSET(fd,&writes))))
                    rfl_io_finish(w,RFL_IO_READY,0);
                else step(w,now);
            }else step(w,now);
        }
        if(w->status&&!w->notified){w->notified=1;events[++events[0]]=i;}
    }
    *sample=now;return 0;
}
void *rfl_poll_connect(void *p,const unsigned char *ip,unsigned port) {
    RflSocket *s;RflWait *w;
    if(!port){errno=EINVAL;return NULL;}
    s=rfl_socket_tcp();if(!s)return NULL;
    w=rfl_io_create(p,s,2,RFL_IO_CONNECT);rfl_socket_release(s);if(!w)return NULL;
    w->peer.sin_family=AF_INET;w->peer.sin_port=htons(port);
    w->peer.sin_addr.s_addr=(unsigned long)ip[0]|((unsigned long)ip[1]<<8)|((unsigned long)ip[2]<<16)|((unsigned long)ip[3]<<24);
    return w;
}
void *rfl_poll_connect_result(void *p) {
    RflWait *w=p;RflSocket *s=w->result_socket;
    if(w->kind!=RFL_IO_CONNECT||w->status!=RFL_IO_READY||!s){errno=EINVAL;return NULL;}
    w->result_socket=NULL;return s;
}
void *rfl_poll_transfer(void *p,void *socket,unsigned kind,const unsigned char *data,unsigned long count) {
    RflWait *w;RflSocket *s=socket;
    if((kind!=RFL_IO_READ&&kind!=RFL_IO_WRITE)||count>(kind==RFL_IO_READ?32767UL:65536UL)){errno=EINVAL;return NULL;}
    if(kind==RFL_IO_READ?s->reading:s->writing){errno=EBUSY;return NULL;}
    w=rfl_io_create(p,socket,kind==RFL_IO_READ?1:2,kind);if(!w)return NULL;
    if(kind==RFL_IO_READ)s->reading=1;else s->writing=1;
    if(count){w->buffer=malloc(count);if(!w->buffer){rfl_poll_drop(w);errno=ENOMEM;return NULL;}}
    w->size=count;if(kind==RFL_IO_WRITE&&count)memcpy(w->buffer,data,count);return w;
}
unsigned long rfl_poll_size(void *p) {return ((RflWait *)p)->offset;}
int rfl_poll_copy(void *p,unsigned char *out,unsigned long capacity) {
    RflWait *w=p;
    if(w->kind!=RFL_IO_READ||w->status!=RFL_IO_READY||capacity<w->offset){errno=EINVAL;return -1;}
    if(w->offset)memcpy(out,w->buffer,w->offset);return 0;
}
