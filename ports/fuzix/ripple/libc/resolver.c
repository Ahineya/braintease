/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ripple socket-based resolver. Wire offsets count octets, never C structs. */
#include <netutil.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
int h_errno;
static unsigned serial;
static char last_server[16];
unsigned ripple_dns_u16(const unsigned char *p){return (p[0]<<8)|p[1];}
unsigned long ripple_dns_u32(const unsigned char *p){return (unsigned long)ripple_dns_u16(p)*65536UL+ripple_dns_u16(p+2);}
static void put16(unsigned char *p,unsigned v){p[0]=v>>8;p[1]=v&255;}
static uint32_t ip4(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int normalize(const char *name,char *out) {
    unsigned n=0,label=0,c;
    if(!strcmp(name,".")){strcpy(out,".");return 0;}
    while((c=*name++)!=0) {
        if(c=='.'&&!*name){if(!label)return -1;break;}
        if(n>=253)return -1;
        if(c=='.') {
            if(!label)return -1;
            if(!*name)break;
            label=0;
        } else {
            if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_')||++label>63)return -1;
            c=tolower(c);
        }
        out[n++]=c;
    }
    if(!n||!label)return -1;
    out[n]=0;return 0;
}
int ripple_dns_name(const unsigned char *p,unsigned length,unsigned *offset,char *out) {
    unsigned at=*offset,next=0,n=0,steps=0,count,target,i,c;
    while(at<length&&++steps<=length) {
        count=p[at++];
        if(!count) {out[n]=0;if(!n)strcpy(out,".");*offset=next?next:at;return 0;}
        if(count>=192) {
            if(at>=length)return -1;
            target=(count&63)*256+p[at++];
            if(target<12||target>=at-2)return -1;
            if(!next)next=at;at=target;
        } else {
            if(count>63||count>length-at)return -1;
            if(n){if(n>=253)return -1;out[n++]='.';}
            if(count>253-n)return -1;
            for(i=0;i<count;i++) {
                c=p[at++];
                if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return -1;
                out[n++]=tolower(c);
            }
        }
    }
    return -1;
}
int ripple_dns_rr(const unsigned char *p,unsigned length,unsigned *offset,struct ripple_dns_rr *rr) {
    unsigned at=*offset,end,i;char name[254];
    if(ripple_dns_name(p,length,&at,rr->name)||length-at<10)return -1;
    rr->type=ripple_dns_u16(p+at);rr->klass=ripple_dns_u16(p+at+2);
    rr->ttl=ripple_dns_u32(p+at+4);rr->size=ripple_dns_u16(p+at+8);rr->data=at+10;
    if(rr->size>length-rr->data)return -1;
    end=rr->data+rr->size;at=rr->data;
    if(rr->klass==1) {
        if((rr->type==1&&rr->size!=4)||(rr->type==28&&rr->size!=16))return -1;
        if(rr->type==2||rr->type==5||rr->type==12) {
            if(ripple_dns_name(p,length,&at,name)||at!=end)return -1;
        } else if(rr->type==15) {
            if(rr->size<3)return -1;at+=2;
            if(ripple_dns_name(p,length,&at,name)||at!=end)return -1;
        } else if(rr->type==6) {
            if(ripple_dns_name(p,length,&at,name)||ripple_dns_name(p,length,&at,name)||at>end||end-at!=20)return -1;
        } else if(rr->type==16) {
            while(at<end){i=p[at++];if(i>end-at)return -1;at+=i;}
        }
    }
    *offset=end;return 0;
}
static unsigned servers(uint32_t *ips) {
    FILE *f=fopen("/etc/resolv.conf","r");char line[256],*key,*value;unsigned n=0;
    if(f) {
        while(n<3&&fgets(line,sizeof(line),f)) {
            key=strtok(line," \t\r\n");value=strtok(0," \t\r\n");
            if(key&&value&&!strcmp(key,"nameserver")&&
               inet_pton(AF_INET,value,&ips[n])==1)++n;
        }
        fclose(f);
    }
    if(!n){inet_pton(AF_INET,"1.1.1.1",ips);n=1;}
    return n;
}
int ripple_dns_server(char *out){uint32_t ips[3];if(*last_server){strcpy(out,last_server);return 0;}servers(ips);return inet_ntop(AF_INET,ips,out,16)?0:-1;}
static int question(const char *name,unsigned type,unsigned char *p,unsigned id) {
    unsigned at=12,start,count;
    memset(p,0,12);put16(p,id);p[2]=1;p[5]=1;
    if(strcmp(name,"."))while(*name) {
        start=at++;count=0;
        while(*name&&*name!='.'){p[at++]=*name++;++count;}
        p[start]=count;if(*name)++name;
    }
    p[at++]=0;put16(p+at,type);put16(p+at+2,1);return at+4;
}
/* -1 malformed, 0 unrelated reply, 1 complete, 2 authenticated truncated reply. */
static int response(const unsigned char *p,unsigned n,const char *name,unsigned type,unsigned id) {
    unsigned flags,at=12,i,count;char q[254];struct ripple_dns_rr rr;
    if(n<12||ripple_dns_u16(p)!=id)return 0;
    flags=ripple_dns_u16(p+2);
    if((flags&0xf840U)!=0x8000U||ripple_dns_u16(p+4)!=1)return -1;
    if(ripple_dns_name(p,n,&at,q)||n-at<4)return -1;
    if(strcmp(q,name)||ripple_dns_u16(p+at)!=type||ripple_dns_u16(p+at+2)!=1)return 0;
    if(flags&0x200)return 2;
    at+=4;count=0;
    for(i=6;i<=10;i+=2){if(ripple_dns_u16(p+i)>128)return -1;count+=ripple_dns_u16(p+i);}
    if(count>128)return -1;
    for(i=0;i<count;i++)if(ripple_dns_rr(p,n,&at,&rr))return -1;
    return at==n?1:-1;
}
static int tcp_query(const struct sockaddr_in *peer,const unsigned char *query,unsigned size,
                     unsigned char *reply,unsigned capacity) {
    int fd,r=-1;unsigned char prefix[2];unsigned n;
    fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    if(nu_connect(fd,(const void *)peer,sizeof(*peer),5000UL))goto done;
    put16(prefix,size);
    if(nu_write(fd,prefix,2,5000UL)||nu_write(fd,query,size,5000UL)||nu_exact(fd,prefix,2,5000UL))goto done;
    n=ripple_dns_u16(prefix);
    if(n<12||n>capacity){errno=EMSGSIZE;goto done;}
    if(nu_exact(fd,reply,n,5000UL))goto done;
    r=n;
done:close(fd);return r;
}
int ripple_dns_query(const char *server,const char *name,unsigned type,unsigned char *reply,unsigned capacity) {
    uint32_t ips[3];unsigned ns,i,attempt,id,size;unsigned char query[272];
    char normalized[254];struct sockaddr_in peer;unsigned long start;int fd,n,r;
    if(!name||!reply||capacity<12||capacity>RIPPLE_DNS_PACKET||!type||normalize(name,normalized)) {
        errno=EINVAL;h_errno=NO_RECOVERY;return -1;
    }
    if(server){if(inet_pton(AF_INET,server,ips)!=1){errno=EINVAL;h_errno=NO_RECOVERY;return -1;}ns=1;}
    else ns=servers(ips);
    id=(getpid()*251)^((unsigned)nu_now())^++serial;size=question(normalized,type,query,id);
    for(i=0;i<ns;i++) {
        memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(53);peer.sin_addr.s_addr=ips[i];
        fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)break;
        if(connect(fd,(void *)&peer,sizeof(peer))||fcntl(fd,F_SETFL,O_NONBLOCK)<0){close(fd);continue;}
        for(attempt=0;attempt<2;attempt++) {
            start=nu_now();
            do {
                n=write(fd,query,size);
                if(n==size)break;
                if(n>=0){errno=EIO;break;}
                if(errno!=EAGAIN||nu_expired(start,2000UL)||nu_ready(fd,1,start,2000UL))break;
            }while(1);
            if(n!=size){if(errno==EINTR){close(fd);h_errno=TRY_AGAIN;return -1;}continue;}
            while(!nu_expired(start,2000UL)) {
                n=read(fd,reply,capacity);
                if(n<0) {
                    if(errno==EAGAIN){if(!nu_ready(fd,0,start,2000UL))continue;}
                    if(errno==EINTR){close(fd);h_errno=TRY_AGAIN;return -1;}
                    break;
                }
                r=response(reply,n,normalized,type,id);
                if(r==2) {
                    close(fd);n=tcp_query(&peer,query,size,reply,capacity);
                    if(n>=0&&response(reply,n,normalized,type,id)==1){inet_ntop(AF_INET,&peer.sin_addr,last_server,16);h_errno=0;return n;}
                    h_errno=TRY_AGAIN;if(n>=0)errno=EINVAL;return -1;
                }
                if(r==1) {
                    /* Try another configured server after transient errors. */
                    if(!server&&i+1<ns&&((reply[3]&15)==2||(reply[3]&15)==5))break;
                    inet_ntop(AF_INET,&peer.sin_addr,last_server,16);close(fd);h_errno=0;return n;
                }
                /* Invalid/unrelated replies never extend the request deadline. */
            }
        }
        close(fd);
    }
    h_errno=TRY_AGAIN;return -1;
}
int res_query(const char *name,int klass,int type,unsigned char *reply,int size) {
    int n;
    if(klass!=1||size<12){errno=EINVAL;h_errno=NO_RECOVERY;return -1;}
    n=ripple_dns_query(0,name,type,reply,size);if(n<0)return -1;
    if(reply[3]&15){h_errno=(reply[3]&15)==3?HOST_NOT_FOUND:(reply[3]&15)==2?TRY_AGAIN:NO_RECOVERY;return -1;}
    h_errno=0;return n;
}
static char hostname[254],*aliases[2],*addresses[5];
static uint32_t values[4];
static struct hostent host;
static struct hostent *result(const char *name,unsigned count) {
    strcpy(hostname,name);host.h_name=hostname;host.h_aliases=aliases;aliases[0]=0;
    host.h_addrtype=AF_INET;host.h_length=sizeof(struct in_addr);host.h_addr_list=addresses;
    addresses[count]=0;h_errno=0;return &host;
}
static struct hostent *hosts(const char *name,const uint32_t *address) {
    FILE *f=fopen("/etc/hosts","r");char line[512],canonical[254],wanted[254],*p,*word;uint32_t value;
    if(!f)return 0;
    while(fgets(line,sizeof(line),f)) {
        p=strchr(line,'#');if(p)*p=0;
        word=strtok(line," \t\r\n");if(!word||inet_pton(AF_INET,word,&value)!=1)continue;
        word=strtok(0," \t\r\n");if(!word||normalize(word,canonical))continue;
        if(address&&value==*address) {
            fclose(f);values[0]=value;addresses[0]=(char *)values;return result(canonical,1);
        }
        if(address)continue;
        do {
            if(!normalize(word,wanted)&&!strcmp(name,wanted)) {
                fclose(f);values[0]=value;addresses[0]=(char *)values;return result(canonical,1);
            }
        }while((word=strtok(0," \t\r\n"))!=0);
    }
    fclose(f);return 0;
}
/* Small positive A cache, local to the process. A CNAME chain's minimum TTL
 * governs its alias, zero TTL is never cached, and resolver changes invalidate
 * all entries. Monotonic elapsed subtraction also handles the clock word wrap. */
#define DNS_CACHE_SLOTS 8
struct dns_cache_entry {
    char name[254],canonical[254];uint32_t values[4];
    unsigned count;unsigned long stamp,lifetime;
};
static struct dns_cache_entry dns_cache[DNS_CACHE_SLOTS];
static uint32_t cache_servers[3];static unsigned cache_server_count,cache_next;
static struct hostent *cached(const char *name) {
    uint32_t ips[3]={0,0,0};unsigned ns=servers(ips),i,j;unsigned long now=nu_now();
    if(ns!=cache_server_count||memcmp(ips,cache_servers,sizeof(ips))) {
        memset(dns_cache,0,sizeof(dns_cache));memcpy(cache_servers,ips,sizeof(ips));cache_server_count=ns;
    }
    for(i=0;i<DNS_CACHE_SLOTS;i++) {
        struct dns_cache_entry *entry=dns_cache+i;
        if(!entry->count||strcmp(name,entry->name))continue;
        if(now-entry->stamp>=entry->lifetime){entry->count=0;return NULL;}
        memcpy(values,entry->values,sizeof(values));
        for(j=0;j<entry->count;j++)addresses[j]=(char *)(values+j);
        return result(entry->canonical,entry->count);
    }
    return NULL;
}
static void cache_result(const char *name,const char *canonical,unsigned count,unsigned long ttl) {
    struct dns_cache_entry *entry;
    if(!ttl)return;
    entry=dns_cache+(cache_next++%DNS_CACHE_SLOTS);
    strcpy(entry->name,name);strcpy(entry->canonical,canonical);
    memcpy(entry->values,values,sizeof(values));entry->count=count;
    entry->stamp=nu_now();entry->lifetime=(ttl>300UL?300UL:ttl)*1000UL;
}
struct hostent *gethostbyname(const char *name) {
    unsigned char packet[RIPPLE_DNS_PACKET];char current[254],original[254],target[254],seen[8][254];
    unsigned long ttl=0xffffffffUL;
    unsigned hop,i,at,count,have,j,advanced;int n;struct ripple_dns_rr rr;struct hostent *h;
    if(!name){h_errno=NO_RECOVERY;return 0;}
    if(inet_pton(AF_INET,name,values)==1){addresses[0]=(char *)values;return result(name,1);}
    if(normalize(name,current)){h_errno=NO_RECOVERY;return 0;}
    if((h=hosts(current,0))!=0)return h;
    if((h=cached(current))!=NULL)return h;
    strcpy(original,current);
    for(hop=0;hop<8;hop++) {
        strcpy(seen[hop],current);n=res_query(current,1,1,packet,sizeof(packet));if(n<0)return 0;
        at=12;if(ripple_dns_name(packet,n,&at,target))break;at+=4;
        count=ripple_dns_u16(packet+6);have=0;target[0]=0;advanced=0;
        /* Follow only records owned by our exact current name. Rescan the
         * same answer for each CNAME so arbitrary answer order is supported. */
        for(j=0;j<8;j++) {
            unsigned scan=at,k;target[0]=0;
            for(i=0;i<count;i++) {
                if(ripple_dns_rr(packet,n,&scan,&rr))goto malformed;
                if(rr.klass!=1||strcmp(rr.name,current))continue;
                if(rr.type==1&&have<4){if(rr.ttl<ttl)ttl=rr.ttl;values[have]=ip4(packet+rr.data);addresses[have]=(char *)&values[have];++have;}
                if(rr.type==5){if(rr.ttl<ttl)ttl=rr.ttl;k=rr.data;if(ripple_dns_name(packet,n,&k,target))goto malformed;}
            }
            if(have){cache_result(original,current,have,ttl);return result(current,have);}
            if(!*target)break;
            for(i=0;i<=hop;i++)if(!strcmp(target,seen[i]))goto malformed;
            if(!strcmp(current,target))goto malformed;
            strcpy(current,target);advanced=1;
        }
        if(j==8)goto malformed;
        if(!advanced){h_errno=NO_DATA;return 0;}
    }
malformed:h_errno=NO_RECOVERY;errno=EINVAL;return 0;
}
struct hostent *gethostbyaddr(const void *p,socklen_t size,int family) {
    unsigned char packet[RIPPLE_DNS_PACKET];char name[80],target[254];unsigned at,count,i;int n;
    struct ripple_dns_rr rr;uint32_t ip;struct hostent *h;
    if(!p||size!=sizeof(ip)||family!=AF_INET){h_errno=NO_RECOVERY;return 0;}
    ip=*(const uint32_t *)p;
    if((h=hosts(0,&ip))!=0)return h;
    sprintf(name,"%u.%u.%u.%u.in-addr.arpa",(unsigned)(ip>>24),(unsigned)((ip>>16)&255),(unsigned)((ip>>8)&255),(unsigned)(ip&255));
    n=res_query(name,1,12,packet,sizeof(packet));if(n<0)return 0;
    at=12;if(ripple_dns_name(packet,n,&at,target))goto bad;at+=4;count=ripple_dns_u16(packet+6);
    for(i=0;i<count;i++) {
        if(ripple_dns_rr(packet,n,&at,&rr))goto bad;
        if(rr.type==12&&rr.klass==1&&!strcmp(rr.name,name)) {
            unsigned data=rr.data;if(ripple_dns_name(packet,n,&data,target))goto bad;
            values[0]=ip;addresses[0]=(char *)values;return result(target,1);
        }
    }
    h_errno=NO_DATA;return 0;
bad:h_errno=NO_RECOVERY;return 0;
}
const char *hstrerror(int e) {
    if(e==HOST_NOT_FOUND)return "Host not found";
    if(e==TRY_AGAIN)return "DNS server unavailable or query timed out";
    if(e==NO_DATA)return "No matching address";
    return "Invalid DNS name or response";
}
void herror(const char *s){fprintf(stderr,"%s: %s\n",s,hstrerror(h_errno));}
