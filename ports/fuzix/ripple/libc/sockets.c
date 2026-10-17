/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
extern long call(unsigned, long, long, long, long);
static int netcall(long op, long a, long b, long c, long d, long e, long f) {
    long args[7];
    args[0]=op; args[1]=a; args[2]=b; args[3]=c; args[4]=d; args[5]=e; args[6]=f;
    return call(66, (long)args, 0, 0, 0);
}
int socket(int domain,int type,int protocol) { return netcall(0,domain,type,protocol,0,0,0); }
int listen(int fd,int backlog) { return netcall(1,fd,backlog,0,0,0,0); }
int bind(int fd,const struct sockaddr *a,socklen_t n) { return netcall(2,fd,(long)a,n,0,0,0); }
int connect(int fd,const struct sockaddr *a,socklen_t n) { return netcall(3,fd,(long)a,n,0,0,0); }
int accept(int fd,struct sockaddr *a,socklen_t *n) { return netcall(4,fd,(long)a,(long)n,0,0,0); }
int getsockname(int fd,struct sockaddr *a,socklen_t *n) { return netcall(5,fd,(long)a,(long)n,0,0,0); }
ssize_t sendto(int fd,const void *b,size_t n,int flags,const struct sockaddr *a,socklen_t alen) {
    if(n>32767UL) { errno=EMSGSIZE;return -1; }
    return netcall(6,fd,(long)b,n,flags,(long)a,alen);
}
ssize_t recvfrom(int fd,void *b,size_t n,int flags,struct sockaddr *a,socklen_t *alen) {
    if(n>32767UL) n=32767UL;
    return netcall(7,fd,(long)b,n,flags,(long)a,(long)alen);
}
ssize_t send(int fd,const void *b,size_t n,int flags) { return sendto(fd,b,n,flags,0,0); }
ssize_t recv(int fd,void *b,size_t n,int flags) { return recvfrom(fd,b,n,flags,0,0); }
int shutdown(int fd,int how) { return netcall(8,fd,how,0,0,0,0); }
int getpeername(int fd,struct sockaddr *a,socklen_t *n) { return netcall(9,fd,(long)a,(long)n,0,0,0); }
uint16_t htons(uint16_t n) { return (n>>8)|(n<<8); }
uint32_t htonl(uint32_t n) {
    return ((n&255UL)<<24)|((n&65280UL)<<8)|((n>>8)&65280UL)|(n>>24);
}
int inet_pton(int family,const char *text,void *out) {
    unsigned part[4],i,n;
    uint32_t address=0;
    if(family!=AF_INET) {errno=EAFNOSUPPORT;return -1;}
    if(!text||!out)return 0;
    for(i=0;i<4;i++) {
        n=0;if(*text<'0'||*text>'9')return 0;
        do {n=n*10+*text++-'0';if(n>255)return 0;}while(*text>='0'&&*text<='9');
        part[i]=n;
        if(i<3&&*text++!='.')return 0;
    }
    if(*text)return 0;
    for(i=0;i<4;i++)address|=(uint32_t)part[i]<<(i*8);
    *(uint32_t *)out=address;return 1;
}
int inet_aton(const char *s,struct in_addr *a) {return inet_pton(AF_INET,s,&a->s_addr)==1;}
in_addr_t inet_addr(const char *s) {in_addr_t n;return inet_pton(AF_INET,s,&n)==1?n:INADDR_NONE;}
const char *inet_ntop(int family,const void *a,char *out,socklen_t size) {
    uint32_t n;char b[16];unsigned len;
    if(family!=AF_INET) {errno=EAFNOSUPPORT;return 0;}
    n=*(const uint32_t *)a;
    sprintf(b,"%u.%u.%u.%u",(unsigned)(n&255),(unsigned)((n>>8)&255),
            (unsigned)((n>>16)&255),(unsigned)(n>>24));
    len=strlen(b)+1;
    if(size<0||(unsigned)size<len){errno=ENOSPC;return 0;}
    memcpy(out,b,len);return out;
}
char *_inet_ntoa(uint32_t n) {static char b[16];inet_ntop(AF_INET,&n,b,16);return b;}
