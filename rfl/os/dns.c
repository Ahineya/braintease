/* Nonblocking IPv4 DNS operations for the guest I/O engine. Reuse libc's bounded
 * wire parsers, but never call its blocking query/gethostbyname network path. */
#include "socket.h"
#include <resolv.h>
#include <stdio.h>
#include <ctype.h>
#define UDP_SEND 0
#define UDP_READ 1
#define TCP_CONNECT 2
#define TCP_SEND 3
#define TCP_PREFIX 4
#define TCP_BODY 5
struct RflDns {
    int fd;
    unsigned phase, initialized, server, servers, attempt, id, query_size;
    unsigned sent, received, response_size, hops;
    unsigned long phase_end;
    uint32_t ips[3], result;
    char name[254], seen[8][254];
    unsigned char query[272], packet[RIPPLE_DNS_PACKET+1], prefix[2];
};
static unsigned serial;
static int normalize(const char *name,char *out) {
    unsigned n=0,label=0,c;
    while((c=*name++)!=0){
        if(c=='.'&&!*name){if(!label)return -1;break;}
        if(n>=253)return -1;
        if(c=='.'){if(!label)return -1;label=0;}
        else{
            if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')||++label>63)return -1;
            c=tolower(c);
        }
        out[n++]=c;
    }
    if(!n||!label)return -1;out[n]=0;return 0;
}
static void put16(unsigned char *p,unsigned n) {p[0]=(n>>8)&255;p[1]=n&255;}
static uint32_t ip4(const unsigned char *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static unsigned servers(uint32_t *ips) {
    FILE *f=fopen("/etc/resolv.conf","r");char line[256],*key,*value;unsigned n=0,lines=0;
    if(f){
        while(n<3&&lines++<64&&fgets(line,sizeof(line),f)){
            key=strtok(line," \t\r\n");value=strtok(NULL," \t\r\n");
            if(key&&value&&!strcmp(key,"nameserver")&&inet_pton(AF_INET,value,&ips[n])==1)++n;
        }
        fclose(f);
    }
    if(!n){inet_pton(AF_INET,"1.1.1.1",ips);n=1;}return n;
}
static int hosts(RflDns *d) {
    FILE *f=fopen("/etc/hosts","r");char line[512],name[254],*p,*word;uint32_t ip;unsigned lines=0;
    if(!f)return 0;
    while(lines++<256&&fgets(line,sizeof(line),f)){
        p=strchr(line,'#');if(p)*p=0;
        word=strtok(line," \t\r\n");if(!word||inet_pton(AF_INET,word,&ip)!=1)continue;
        while((word=strtok(NULL," \t\r\n"))!=NULL){
            if(!normalize(word,name)&&!strcmp(name,d->name)){d->result=ip;fclose(f);return 1;}
        }
    }
    fclose(f);return 0;
}
static int question(RflDns *d) {
    unsigned at=12,start,count;const char *s=d->name;
    memset(d->query,0,12);put16(d->query,d->id);d->query[2]=1;d->query[5]=1;
    while(*s){
        start=at++;count=0;while(*s&&*s!='.'){d->query[at++]=*s++;++count;}
        d->query[start]=count;if(*s)++s;
    }
    d->query[at++]=0;put16(d->query+at,1);put16(d->query+at+2,1);d->query_size=at+4;return 0;
}
void rfl_dns_close(RflWait *w) {if(w->dns->fd>=0){close(w->dns->fd);w->dns->fd=-1;}}
void rfl_dns_destroy(RflWait *w) {rfl_dns_close(w);free(w->dns);w->dns=NULL;}
int rfl_dns_fd(RflWait *w) {return w->dns->fd;}
unsigned rfl_dns_events(RflWait *w) {
    unsigned phase=w->dns->phase;return phase==UDP_SEND||phase==TCP_CONNECT||phase==TCP_SEND?2:1;
}
unsigned long rfl_dns_delay(RflWait *w,unsigned long now,unsigned long delay) {
    unsigned long left;
    if(!w->dns->initialized)return 0;
    left=rfl_io_remaining(now,w->dns->phase_end);return left<delay?left:delay;
}
static void peer(RflDns *d,struct sockaddr_in *address) {
    memset(address,0,sizeof(*address));address->sin_family=AF_INET;
    address->sin_port=htons(53);address->sin_addr.s_addr=d->ips[d->server];
}
static int udp(RflWait *w,unsigned long now) {
    RflDns *d=w->dns;struct sockaddr_in address;int saved;
    rfl_dns_close(w);d->fd=socket(AF_INET,SOCK_DGRAM,0);
    if(d->fd<0)return -1;
    peer(d,&address);
    if(fcntl(d->fd,F_SETFL,O_NONBLOCK)<0||connect(d->fd,(void *)&address,sizeof(address))<0){
        saved=errno;rfl_dns_close(w);errno=saved;return -1;
    }
    d->phase=UDP_SEND;d->phase_end=now+2000;return 0;
}
static void next_server(RflWait *w,unsigned long now) {
    RflDns *d=w->dns;
    if(++d->attempt==2){d->attempt=0;++d->server;}
    if(d->server>=d->servers){rfl_io_finish(w,RFL_IO_DNS_ERROR,TRY_AGAIN);return;}
    if(udp(w,now)<0)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);
}
static int tcp(RflWait *w,unsigned long now) {
    RflDns *d=w->dns;struct sockaddr_in address;int n,saved;
    rfl_dns_close(w);d->fd=socket(AF_INET,SOCK_STREAM,0);if(d->fd<0)return -1;
    peer(d,&address);
    if(fcntl(d->fd,F_SETFL,O_NONBLOCK)<0){saved=errno;rfl_dns_close(w);errno=saved;return -1;}
    n=connect(d->fd,(void *)&address,sizeof(address));
    if(n<0&&errno!=EINPROGRESS&&errno!=EALREADY&&errno!=EAGAIN&&errno!=EINTR&&errno!=EISCONN){
        saved=errno;rfl_dns_close(w);errno=saved;return -1;
    }
    d->phase=n<0&&errno!=EISCONN?TCP_CONNECT:TCP_SEND;d->phase_end=now+5000;
    d->sent=d->received=0;put16(d->prefix,d->query_size);return 0;
}
/* 0 unrelated/malformed, 1 validated response, 2 truncated matching response. */
static int validate(RflDns *d,unsigned n) {
    unsigned at=12,flags,i,count=0;char q[254];struct ripple_dns_rr rr;
    if(n<12||ripple_dns_u16(d->packet)!=d->id)return 0;
    flags=ripple_dns_u16(d->packet+2);
    if((flags&0xf840U)!=0x8000U||ripple_dns_u16(d->packet+4)!=1)return 0;
    if(ripple_dns_name(d->packet,n,&at,q)||n-at<4||strcmp(q,d->name)||
        ripple_dns_u16(d->packet+at)!=1||ripple_dns_u16(d->packet+at+2)!=1)return 0;
    if(flags&0x200)return 2;
    at+=4;
    for(i=6;i<=10;i+=2){if(ripple_dns_u16(d->packet+i)>128)return 0;count+=ripple_dns_u16(d->packet+i);}
    if(count>128)return 0;
    for(i=0;i<count;i++)if(ripple_dns_rr(d->packet,n,&at,&rr))return 0;
    return at==n?1:0;
}
static int follow(RflWait *w,const char *target) {
    RflDns *d=w->dns;unsigned i;
    if(d->hops>=8)return -1;
    for(i=0;i<d->hops;i++)if(!strcmp(d->seen[i],target))return -1;
    strcpy(d->seen[d->hops++],d->name);strcpy(d->name,target);return 0;
}
static void answer(RflWait *w,unsigned n,unsigned long now) {
    RflDns *d=w->dns;struct ripple_dns_rr rr;unsigned code=d->packet[3]&15,at=12,count,i,j,k,scan,advanced=0;
    char target[254];
    if(code){
        if(code==2||code==5){d->attempt=1;next_server(w,now);}
        else rfl_io_finish(w,RFL_IO_DNS_ERROR,code==3?HOST_NOT_FOUND:NO_RECOVERY);
        return;
    }
    if(ripple_dns_name(d->packet,n,&at,target)){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}
    at+=4;count=ripple_dns_u16(d->packet+6);
    for(j=0;j<8;j++){
        target[0]=0;scan=at;
        for(i=0;i<count;i++){
            if(ripple_dns_rr(d->packet,n,&scan,&rr)){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}
            if(rr.klass!=1||strcmp(rr.name,d->name))continue;
            if(rr.type==1){d->result=ip4(d->packet+rr.data);rfl_io_finish(w,RFL_IO_READY,0);return;}
            if(rr.type==5){k=rr.data;if(ripple_dns_name(d->packet,n,&k,target)){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}}
        }
        if(!*target)break;
        ++advanced;
        if(follow(w,target)){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}
    }
    if(j==8){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}
    if(!advanced){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_DATA);return;}
    /* No A record for the CNAME target in this packet. Query it with a new ID;
     * the overall operation deadline and visited-name set remain unchanged. */
    ++d->id;question(d);d->server=d->attempt=0;
    if(udp(w,now)<0)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);
}
static int initialize(RflWait *w,unsigned long now) {
    RflDns *d=w->dns;unsigned char entropy[2];int fd,n,saved;
    d->initialized=1;
    if(inet_pton(AF_INET,d->name,&d->result)==1||hosts(d)){rfl_io_finish(w,RFL_IO_READY,0);return 0;}
    d->servers=servers(d->ips);
    fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;
    n=read(fd,entropy,2);saved=errno;close(fd);errno=saved;
    if(n!=2){if(n>=0)errno=EIO;return -1;}
    d->id=((unsigned)entropy[0]<<8)|entropy[1];d->id^=++serial;question(d);
    return udp(w,now);
}
void rfl_dns_step(RflWait *w,unsigned long now) {
    RflDns *d=w->dns;long n;unsigned budget,length;int error; socklen_t size;
    struct sockaddr_in address;
    if(!d->initialized){if(initialize(w,now)<0)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;}
    if(!rfl_io_remaining(now,d->phase_end)){
        if(d->phase<=UDP_READ)next_server(w,now);
        else rfl_io_finish(w,RFL_IO_DNS_ERROR,TRY_AGAIN);
        return;
    }
    if(d->phase==UDP_SEND){
        n=send(d->fd,d->query,d->query_size,0);
        if(n==d->query_size)d->phase=UDP_READ;
        else if(n>=0)rfl_io_finish(w,RFL_IO_OS_ERROR,EIO);
        else if(errno!=EAGAIN&&errno!=EINTR)next_server(w,now);
        return;
    }
    if(d->phase==UDP_READ){
        for(budget=0;budget<4;budget++){
            n=recv(d->fd,d->packet,sizeof(d->packet),0);
            if(n<0){if(errno!=EAGAIN&&errno!=EINTR)next_server(w,now);return;}
            if(n>RIPPLE_DNS_PACKET)continue;
            length=validate(d,n);
            if(length==2){if(tcp(w,now)<0)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;}
            if(length==1){answer(w,n,now);return;}
        }
        return;
    }
    if(d->phase==TCP_CONNECT){
        error=0;size=sizeof(error);
        if(getsockopt(d->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0){rfl_io_finish(w,RFL_IO_OS_ERROR,errno);return;}
        if(error){rfl_io_finish(w,RFL_IO_OS_ERROR,error);return;}
        size=sizeof(address);
        if(getpeername(d->fd,(void *)&address,&size)<0){
            if(errno!=ENOTCONN&&errno!=EINPROGRESS)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);
            return;
        }
        d->phase=TCP_SEND;return;
    }
    if(d->phase==TCP_SEND){
        if(d->sent<2)n=send(d->fd,d->prefix+d->sent,2-d->sent,0);
        else n=send(d->fd,d->query+d->sent-2,d->query_size+2-d->sent,0);
        if(n>0){d->sent+=n;if(d->sent==d->query_size+2){d->phase=TCP_PREFIX;d->received=0;}return;}
    }else if(d->phase==TCP_PREFIX){
        n=recv(d->fd,d->prefix+d->received,2-d->received,0);
        if(n>0){
            d->received+=n;
            if(d->received==2){
                d->response_size=ripple_dns_u16(d->prefix);d->received=0;
                if(d->response_size<12||d->response_size>RIPPLE_DNS_PACKET){rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);return;}
                d->phase=TCP_BODY;
            }
            return;
        }
    }else{
        n=recv(d->fd,d->packet+d->received,d->response_size-d->received,0);
        if(n>0){
            d->received+=n;
            if(d->received==d->response_size){
                if(validate(d,d->received)==1)answer(w,d->received,now);
                else rfl_io_finish(w,RFL_IO_DNS_ERROR,NO_RECOVERY);
            }
            return;
        }
    }
    if(n==0)rfl_io_finish(w,RFL_IO_OS_ERROR,EIO);
    else if(errno!=EAGAIN&&errno!=EINTR)rfl_io_finish(w,RFL_IO_OS_ERROR,errno);
}
void *rfl_poll_resolve(void *p,const unsigned char *name) {
    RflWait *w;RflDns *d;
    d=calloc(1,sizeof(*d));if(!d){errno=ENOMEM;return NULL;}d->fd=-1;
    if(normalize((const char *)name,d->name)){free(d);errno=EINVAL;return NULL;}
    w=rfl_io_create(p,NULL,1,RFL_IO_DNS);if(!w){free(d);return NULL;}
    w->dns=d;w->has_timeout=1;w->duration=5000;return w;
}
int rfl_poll_resolve_result(void *p,unsigned char *out) {
    RflWait *w=p;uint32_t ip;
    if(w->kind!=RFL_IO_DNS||w->status!=RFL_IO_READY){errno=EINVAL;return -1;}
    ip=w->dns->result;out[0]=ip&255;out[1]=(ip>>8)&255;out[2]=(ip>>16)&255;out[3]=(ip>>24)&255;return 0;
}
