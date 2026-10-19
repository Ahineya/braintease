/* RFL guest sockets and readiness. No host sockets or physical MMIO addresses. */
#include "socket.h"
#include <netdb.h>

int rfl_socket_errno(void) { return errno; }
static void *own(int fd) {
    RflSocket *s; int flags, saved;
    if(fd<0)return NULL;
    flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){saved=errno;close(fd);errno=saved;return NULL;}
    s=malloc(sizeof(*s));
    if(!s){close(fd);errno=ENOMEM;return NULL;}
    s->refs=1;s->fd=fd;s->reading=s->writing=0;return s;
}
void *rfl_socket_tcp(void) { return own(socket(AF_INET,SOCK_STREAM,0)); }
void *rfl_socket_retain(void *p) {
    RflSocket *s=p;
    if(!s||s->refs==0xffffffffUL){errno=EINVAL;return NULL;}
    ++s->refs;return s;
}
void rfl_socket_release(void *p) {
    RflSocket *s=p;
    if(s&&!--s->refs){close(s->fd);free(s);}
}
int rfl_socket_descriptor(void *p) { return ((RflSocket *)p)->fd; }
/* Descriptor watches own dup(), so closing/reusing the caller's fd is safe. */
void *rfl_socket_from_fd(int fd) {
    RflSocket *s; int copy=dup(fd);
    if(copy<0)return NULL;
    s=malloc(sizeof(*s));
    if(!s){close(copy);errno=ENOMEM;return NULL;}
    s->refs=1;s->fd=copy;s->reading=s->writing=0;return s;
}
void *rfl_socket_pair(void) {
    RflSocket **out;int fd[2],saved;
    out=calloc(2,sizeof(*out));if(!out){errno=ENOMEM;return NULL;}
    if(socketpair(AF_UNIX,SOCK_STREAM,0,fd)<0){free(out);return NULL;}
    out[0]=own(fd[0]);
    if(!out[0]){saved=errno;close(fd[1]);free(out);errno=saved;return NULL;}
    out[1]=own(fd[1]);
    if(!out[1]){saved=errno;rfl_socket_release(out[0]);free(out);errno=saved;return NULL;}
    return out;
}
void *rfl_socket_pair_take(void *p,unsigned index) {
    RflSocket **pair=p,*s;
    if(index>1){errno=EINVAL;return NULL;}
    s=pair[index];pair[index]=NULL;return s;
}
void rfl_socket_pair_drop(void *p) {
    RflSocket **pair=p;rfl_socket_release(pair[0]);rfl_socket_release(pair[1]);free(p);
}
int rfl_socket_connect(void *p,const unsigned char *ip,unsigned port) {
    struct sockaddr_in address;int status;
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(port);
    address.sin_addr.s_addr=(unsigned long)ip[0]|((unsigned long)ip[1]<<8)|
        ((unsigned long)ip[2]<<16)|((unsigned long)ip[3]<<24);
    status=connect(((RflSocket *)p)->fd,(void *)&address,sizeof(address));
    if(!status||errno==EISCONN)return 1;
    if(errno==EINPROGRESS||errno==EALREADY||errno==EAGAIN)return 0;
    return -1;
}
int rfl_socket_finish(void *p) {
    int error=0; socklen_t size=sizeof(error),length;struct sockaddr_in address;
    if(getsockopt(((RflSocket *)p)->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)return -1;
    if(error){errno=error;return -1;}
    length=sizeof(address);
    return getpeername(((RflSocket *)p)->fd,(void *)&address,&length);
}
long rfl_socket_read(void *p,unsigned char *data,unsigned long count) {
    long n;
    if(count>32767UL){errno=EINVAL;return -1;}
    if(((RflSocket *)p)->reading){errno=EBUSY;return -1;}
    n=recv(((RflSocket *)p)->fd,data,count,0);
    return n<0&&(errno==EAGAIN||errno==EINTR)?-2:n;
}
long rfl_socket_write(void *p,const unsigned char *data,unsigned long count) {
    long n;
    if(count>32767UL){errno=EINVAL;return -1;}
    if(((RflSocket *)p)->writing){errno=EBUSY;return -1;}
    n=send(((RflSocket *)p)->fd,data,count,0);
    return n<0&&(errno==EAGAIN||errno==EINTR)?-2:n;
}
int rfl_socket_shutdown(void *p,unsigned how) { return shutdown(((RflSocket *)p)->fd,how); }
/* Resolver is the guest libc resolver. Its result storage never escapes. */
int rfl_socket_resolve(const unsigned char *name,unsigned char *out) {
    struct hostent *h=gethostbyname((const char *)name);
    unsigned long ip;
    if(!h)return h_errno?h_errno:-1;
    if(h->h_addrtype!=AF_INET||h->h_length!=sizeof(ip)||!h->h_addr_list[0])return -1;
    memcpy(&ip,h->h_addr_list[0],sizeof(ip));
    out[0]=ip&255;out[1]=(ip>>8)&255;out[2]=(ip>>16)&255;out[3]=(ip>>24)&255;return 0;
}
