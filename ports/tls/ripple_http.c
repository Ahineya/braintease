/* Generic guest HTTP downloader, adapted from FUZIX Applications/netd/htget.c.
 * Ripple adaptation: incremental framing, partial I/O, timeouts, redirects,
 * chunk decoding and temporary-file replacement. No wire struct overlays. */
#include <netutil.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <time.h>
#include <ripple_tls.h>
#include "ripple_http.h"
#include "../../libraries/deflate/gzip.h"
#define LINE 1024
static int sock=-1,out=-1;
static RippleTLS *tls;
static unsigned tls_failed;
static volatile int interrupted;
static char temporary[512];
static unsigned char input[4096],output[4096];
static unsigned head,used,buffered;
static unsigned long saved,response_started;
static unsigned request_used,profile,response_octets;
/* --response is a generic streaming IPC/CLI mode: one response, final headers,
 * dechunked entity, exit status on stderr/errors. No TLS state crosses it. */
static unsigned response_mode, worker_mode, header_count;
static unsigned long body_limit=262144UL;
static char connected_host[254];
static unsigned connected_port, connected_secure;
static const char *failure;
static const char *extra_headers[4];
static char *response_headers;
static unsigned response_used;
static int allowed_header(const char *);
/* Pipe records use serialized octets, never a native C structure. */
static int frame(unsigned kind,const void *data,unsigned length) {
    unsigned char prefix[3];
    prefix[0]=kind;prefix[1]=length>>8;prefix[2]=length&255;
    if(nu_write(1,prefix,3,30000UL))return -1;
    return length?nu_write(1,data,length,30000UL):0;
}
static int emit(unsigned kind,const void *data,unsigned length) {
    return worker_mode?frame(kind,data,length):nu_write(1,data,length,30000UL);
}
/* Idle workers block without a request timeout. The browser owns their
 * lifetime; EOF closes the cached connection and exits. */
static int read_exact(void *data,unsigned length) {
    char *at=data;ssize_t n;unsigned done=0;
    while(done<length) {
        n=read(0,at+done,length-done);
        if(n<0&&errno==EINTR&&!interrupted)continue;
        if(n<=0)return !done&&!n?0:-1;
        done+=n;
    }
    return 1;
}
static int request(char *data) {
    unsigned char prefix[2];unsigned length,i;char *at,*end;
    int r=read_exact(prefix,2);
    if(r<=0)return r;
    if(prefix[0]>255||prefix[1]>255)return -1;
    length=(unsigned)prefix[0]*256+prefix[1];
    if(!length||length>RIPPLE_HTTP_REQUEST_MAX||read_exact(data,length)!=1)return -1;
    /* Embedded NUL/control characters cannot truncate a framed request. */
    for(i=0;i<length;i++)if((data[i]<32&&data[i]!='\n'&&data[i]!='\t')||data[i]>126)return -1;
    data[length]=0;at=strchr(data,'\n');
    if(!at||at==data||at-data>=1280)return -1;
    *at++=0;header_count=0;
    while(at<data+length) {
        end=strchr(at,'\n');if(!end)return -1;*end++=0;
        if(!*at)return end==data+length?1:-1;
        if(header_count==4||!allowed_header(at))return -1;
        extra_headers[header_count++]=at;at=end;
    }
    return -1;
}
static int append_header(const char *s) {
    unsigned n=strlen(s);
    if(response_used>16382||n>16382-response_used){errno=EMSGSIZE;return -1;}
    memcpy(response_headers+response_used,s,n);response_used+=n;
    response_headers[response_used++]='\r';response_headers[response_used++]='\n';return 0;
}
static int allowed_header(const char *s) {
    const char *p=strchr(s,':');unsigned n;
    if(!p||p==s||strlen(s)>LINE)return 0;
    n=p-s;
    if((n==4&&!strncasecmp(s,"Host",n)) || (n==14&&!strncasecmp(s,"Content-Length",n)) ||
       (n==17&&!strncasecmp(s,"Transfer-Encoding",n)) || (n==10&&!strncasecmp(s,"Connection",n)) ||
       (n==15&&!strncasecmp(s,"Accept-Encoding",n)))return 0;
    { const char *at;for(at=s;at<p;at++)if(!isalnum(*at)&&!strchr("!#$%&'*+-.^_`|~",*at))return 0; }
    for(;*s;s++)if((*s<32&&*s!='\t')||*s>126)return 0;
    return 1;
}
static void caught(int sig){interrupted=sig;signal(sig,caught);}
static void disconnect(void){if(tls){ripple_tls_close(tls);tls=NULL;}if(sock>=0){close(sock);sock=-1;}}
static void cleanup(void){if(out>=0)close(out);disconnect();if(*temporary)unlink(temporary);free(response_headers);}
static int request_flush(void) {
    int r;
    if(!request_used)return 0;
    r=tls?ripple_tls_write(tls,output,request_used,30000UL):nu_write(sock,output,request_used,30000UL);
    if(tls&&r<0)tls_failed=1;
    if(!r)request_used=0;return r;
}
static int request_write(const void *p,size_t n) {
    const unsigned char *next=p;unsigned part;
    while(n) {
        part=sizeof(output)-request_used;if((size_t)part>n)part=n;
        memcpy(output+request_used,next,part);request_used+=part;next+=part;n-=part;
        if(request_used==sizeof(output)&&request_flush())return -1;
    }
    return 0;
}
static unsigned long response_clock(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;
}
static void response_report(void) {
    if(profile)fprintf(stderr,"HTTP response detail: %lu body octets, %lu ms\n",saved,response_clock()-response_started);
}
static int flush_body(void);
static int fill_input(void) {
    ssize_t n;
    if(interrupted){errno=EINTR;return -1;}
    if(head!=used)return 1;
    /* Deliver available entity bytes before waiting for another packet. */
    if(response_mode&&buffered&&flush_body())return -1;
    n=tls?ripple_tls_read(tls,input,sizeof(input),30000UL):nu_read(sock,input,sizeof(input),30000UL);
    if(tls&&n<0)tls_failed=1;
    if(n<=0)return n<0?-1:0;
    response_octets=1;head=0;used=n;return 1;
}
static int byte(void) {
    int r=fill_input();
    return r>0?input[head++]:r<0?-1:-2;
}
static int line(char *s,unsigned long *budget) {
    unsigned n=0;int c;
    for(;;) {
        c=byte();if(c<0){if(c==-2)errno=EIO;return -1;}
        if(!*budget){errno=EMSGSIZE;return -1;}--*budget;
        if(c=='\r') {if(byte()!='\n'){errno=EINVAL;return -1;}s[n]=0;return 0;}
        if(c=='\n'||(c<32&&c!='\t')||c>126||n==LINE){errno=EINVAL;return -1;}
        s[n++]=c;
    }
}
static int decimal(const char *s,unsigned long *v) {
    unsigned c;*v=0;if(!*s)return -1;
    while((c=*s++)!=0) {
        if(c<'0'||c>'9'||*v>(2147483647UL-(c-'0'))/10)return -1;
        *v=*v*10+c-'0';
    }
    return 0;
}
static int parse_url(const char *url,char *host,char *path,unsigned *port,unsigned *secure) {
    const char *p,*start;unsigned n=0;char number[8];unsigned long v;
    if(!strncmp(url,"https://",8)){*secure=1;p=url+8;*port=443;}
    else if(!strncmp(url,"http://",7)){*secure=0;p=url+7;*port=80;}
    else return -1;
    start=p;
    while(*p&&*p!='/'&&*p!='?'&&*p!='#'&&*p!=':') {
        if(n==253||*p<=32||*p>126||*p=='@'||*p=='['||*p==']')return -1;
        host[n++]=*p++;
    }
    if(p==start)return -1;host[n]=0;
    if(*p==':') {
        ++p;n=0;while(*p&&*p!='/'&&*p!='?'&&*p!='#'){if(n==7)return -1;number[n++]=*p++;}
        number[n]=0;if(decimal(number,&v)||!v||v>65535UL)return -1;*port=v;
    }
    n=0;if(*p!='/')path[n++]='/';
    while(*p&&*p!='#'){if(n==LINE||*p<=32||*p>126)return -1;path[n++]=*p++;}
    path[n]=0;return 0;
}
static int redirected(const char *url,const char *location,char *next) {
    const char *p,*last;unsigned n,secure=!strncmp(url,"https://",8);
    if(!strncmp(location,"https://",8)||!strncmp(location,"http://",7)) {
        if(secure&&!strncmp(location,"http://",7))return -1;
        if(strlen(location)>1279)return -1;strcpy(next,location);return 0;
    }
    if(strstr(location,"://"))return -1;
    if(location[0]=='/'&&location[1]=='/') {
        if(strlen(location)>1273)return -1;strcpy(next,secure?"https:":"http:");strcat(next,location);return 0;
    }
    p=url+(secure?8:7);while(*p&&*p!='/'&&*p!='?'&&*p!='#')++p;
    if(*location=='/')n=p-url;
    else if(*location=='?') {p=url;while(*p&&*p!='?'&&*p!='#')++p;n=p-url;}
    else {
        last=p;while(*p&&*p!='?'&&*p!='#'){if(*p=='/')last=p;++p;}
        n=last-url+(*last=='/'?1:0);
    }
    if(n+strlen(location)+2>1280)return -1;
    memcpy(next,url,n);next[n]=0;
    if(*location!='/'&&*location!='?'&&next[n-1]!='/')strcat(next,"/");
    strcat(next,location);return 0;
}
static int flush_body(void) {
    if(buffered&&(worker_mode?frame(RIPPLE_HTTP_BODY,output,buffered):nu_write(out,output,buffered,30000UL)))return -1;
    saved+=buffered;buffered=0;return 0;
}
/* Copy known entity spans, leaving byte parsing only to HTTP framing. */
static int body_bytes(unsigned long length) {
    unsigned n,space;
    int r;
    while(length) {
        r=fill_input();
        if(r<=0){if(!r)errno=EIO;return -1;}
        n=used-head;space=sizeof(output)-buffered;
        if(n>space)n=space;if((unsigned long)n>length)n=length;
        if((response_mode&&saved+buffered+n>body_limit)||saved+buffered+n>2147483647UL){errno=EFBIG;return -1;}
        memcpy(output+buffered,input+head,n);
        buffered+=n;head+=n;length-=n;
        if(buffered==sizeof(output)&&flush_body())return -1;
    }
    return 0;
}
/* Transfer framing remains separate from content coding. The inflater reads
 * only entity octets, so it cannot consume the next persistent HTTP response. */
struct compressed_entity {
    unsigned chunked,has_length,after_chunk,finished;
    unsigned long remaining,received,header_budget;
};
static int entity_next(void *opaque) {
    struct compressed_entity *e=opaque;int value;char text[LINE+1],*p;unsigned digit;unsigned long n;
    if(e->finished)return -1;
    if(e->chunked&&!e->remaining) {
        if(e->after_chunk&&(byte()!='\r'||byte()!='\n')){errno=EINVAL;return -2;}
        if(line(text,&e->header_budget))return -2;
        p=text;n=0;if(!isxdigit(*p)){errno=EINVAL;return -2;}
        while(isxdigit(*p)) {
            digit=isdigit(*p)?*p-'0':tolower(*p)-'a'+10;
            if(n>(body_limit-digit)/16){errno=EFBIG;return -2;}n=n*16+digit;++p;
        }
        if(*p&&*p!=';'){errno=EINVAL;return -2;}
        if(!n) {
            do {if(line(text,&e->header_budget))return -2;
                if(*text&&!strchr(text,':')){errno=EINVAL;return -2;}
            }while(*text);
            e->finished=1;return -1;
        }
        e->remaining=n;e->after_chunk=1;
    }
    if((e->chunked||e->has_length)&&!e->remaining){e->finished=1;return -1;}
    value=byte();
    if(value<0) {
        if(value==-2&&!e->chunked&&!e->has_length){e->finished=1;return -1;}
        if(value==-2)errno=EIO;return -2;
    }
    if(e->received==body_limit){errno=EFBIG;return -2;}
    ++e->received;if(e->chunked||e->has_length)--e->remaining;
    return value;
}
static int decoded_body(void *unused,const unsigned char *data,unsigned n) {
    (void)unused;
    if(worker_mode?frame(RIPPLE_HTTP_BODY,data,n):nu_write(out,data,n,30000UL))return -1;
    saved+=n;return 0;
}
static int gzip_body(unsigned chunked,unsigned has_length,unsigned long length) {
    struct compressed_entity e;uint32_t produced=0;int32_t result;
    memset(&e,0,sizeof(e));e.chunked=chunked;e.has_length=has_length;
    e.remaining=chunked?0:length;e.header_budget=65536UL;
    if(has_length&&length>body_limit){errno=EFBIG;return -1;}
    errno=0;
    result=ri_gzip_stream(entity_next,decoded_body,&e,body_limit,body_limit,&produced);
    if(result){if(!errno)errno=result==TINF_BUF_ERROR?EFBIG:EINVAL;return -1;}
    return 0;
}
static int body(unsigned chunked,unsigned has_length,unsigned long length) {
    char text[LINE+1],*p;unsigned long n,budget=16384;unsigned digit;int c;
    if(chunked)for(;;) {
        if(line(text,&budget))return -1;
        n=0;p=text;if(!isxdigit(*p)){errno=EINVAL;return -1;}
        while(isxdigit(*p)) {
            digit=isdigit(*p)?*p-'0':tolower(*p)-'a'+10;
            if(n>(2147483647UL-digit)/16){errno=EFBIG;return -1;}n=n*16+digit;++p;
        }
        if(*p&&*p!=';'){errno=EINVAL;return -1;}
        if(!n) {
            do {if(line(text,&budget))return -1;if(*text&&!strchr(text,':')){errno=EINVAL;return -1;}}while(*text);
            break;
        }
        if(body_bytes(n))return -1;
        if(byte()!='\r'||byte()!='\n'){errno=EINVAL;return -1;}
        /* Bound each chunk-size/trailer block, without limiting total body. */
        budget=16384;
    } else if(has_length) {
        if(body_bytes(length))return -1;
    } else {
        while((c=fill_input())>0)if(body_bytes(used-head))return -1;
        if(c<0)return -1;
    }
    return flush_body();
}
static int output_file(const char *name) {
    const char *slash;unsigned n;
    if(!strcmp(name,"-")){out=1;return 0;}
    slash=strrchr(name,'/');n=slash?(unsigned)(slash-name+1):0;
    if(n+14>=sizeof(temporary)){errno=ENAMETOOLONG;return -1;}
    memcpy(temporary,name,n);strcpy(temporary+n,".htgetXXXXXX");
    out=mkstemp(temporary);return out<0?-1:0;
}
static int download(const char *address,const char *destination,const char *ca_file) {
    char url[1280],next[1280],host[254],path[LINE+1],text[LINE+1],location[LINE+1],*value,*end;
    unsigned port,secure,redirects=0,status,info,has_length,chunked,fields,close_after,reused,gzip,have_encoding;
    unsigned long length,budget;struct hostent *h;struct sockaddr_in peer;
    saved=buffered=request_used=head=used=response_used=response_octets=0;failure=NULL;tls_failed=0;
    if(strlen(address)>=sizeof(url))goto invalid;
    strcpy(url,address);
    for(;;) {
        if(parse_url(url,host,path,&port,&secure))goto invalid;
        reused=worker_mode&&sock>=0&&connected_port==port&&connected_secure==secure&&!strcasecmp(host,connected_host);
        if(!reused)disconnect();
retry:
        if(sock<0) {
        h=gethostbyname(host);if(!h){failure="Hostname lookup failed";goto error;}
        memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(port);
        memcpy(&peer.sin_addr,h->h_addr_list[0],sizeof(peer.sin_addr));
        sock=socket(AF_INET,SOCK_STREAM,0);if(sock<0||nu_connect(sock,(void *)&peer,sizeof(peer),30000UL))goto failed;
        if(secure) {
            /* Cold JIT compilation and guest public-key arithmetic count too. */
            tls=ripple_tls_open(sock,host,ca_file,120000UL,&interrupted);
            if(!tls){tls_failed=1;goto failed;}
        }
        strcpy(connected_host,host);connected_port=port;connected_secure=secure;
        }
        if(request_write("GET ",4)||request_write(path,strlen(path))||
           request_write(" HTTP/1.1\r\nHost: ",17)||request_write(host,strlen(host)))goto stale;
        if(port!=(secure?443:80)){sprintf(text,":%u",port);if(request_write(text,strlen(text)))goto stale;}
        strcpy(text,"\r\nUser-Agent: Ripple-htget/0.4\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
        if(worker_mode)strcpy(text,"\r\nUser-Agent: Ripple-htget/0.4\r\nAccept-Encoding: gzip\r\nConnection: keep-alive\r\n");
        if(request_write(text,strlen(text)))goto stale;
        for(fields=0;fields<header_count;fields++)
            if(request_write(extra_headers[fields],strlen(extra_headers[fields]))||request_write("\r\n",2))goto stale;
        if(request_write("\r\n",2)||request_flush())goto stale;
        response_started=profile?response_clock():0;
        head=used=0;budget=16384;info=0;
        do {
            response_used=0;
            if(line(text,&budget))goto stale;
            reused=0; /* Never retry after receiving a response status line. */
            close_after=!strncmp(text,"HTTP/1.0 ",9);
            if(response_mode&&append_header(text))goto failed;
            if((strncmp(text,"HTTP/1.0 ",9)&&strncmp(text,"HTTP/1.1 ",9))||strlen(text)<12||
               !isdigit(text[9])||!isdigit(text[10])||!isdigit(text[11])||(text[12]&&text[12]!=' '))goto malformed;
            status=(text[9]-'0')*100+(text[10]-'0')*10+text[11]-'0';
            if(status<100||status>599||status==101)goto malformed;
            has_length=chunked=fields=gzip=have_encoding=0;length=0;location[0]=0;
            for(;;) {
                if(line(text,&budget))goto failed;
                if(!*text)break;if(++fields>128)goto malformed;
                /* Entity output is already dechunked. A standard reader can
                 * consume it to EOF; do not advertise chunk framing on stdout. */
                if(response_mode&&strncasecmp(text,"Transfer-Encoding:",18)&&
                   strncasecmp(text,"Content-Length:",15)&&strncasecmp(text,"Content-Encoding:",17)&&append_header(text))goto failed;
                value=strchr(text,':');if(!value||value==text)goto malformed;*value++=0;
                for(end=text;*end;end++)if(!isalnum(*end)&&!strchr("!#$%&'*+-.^_`|~",*end))goto malformed;
                while(*value==' '||*value=='\t')++value;
                end=value+strlen(value);while(end>value&&(end[-1]==' '||end[-1]=='\t'))*--end=0;
                if(!strcasecmp(text,"Content-Length")){if(has_length||decimal(value,&length))goto malformed;has_length=1;}
                else if(!strcasecmp(text,"Transfer-Encoding")){if(chunked||strcasecmp(value,"chunked"))goto malformed;chunked=1;}
                else if(!strcasecmp(text,"Content-Encoding")) {
                    if(have_encoding++)goto malformed;
                    if(!strcasecmp(value,"gzip"))gzip=1;
                    else if(strcasecmp(value,"identity"))goto malformed;
                }
                else if(!strcasecmp(text,"Connection")) {
                    const char *part=value,*stop;
                    while(*part) {
                        while(*part==' '||*part=='\t'||*part==',')part++;
                        stop=part;while(*stop&&*stop!=','&&*stop!=' '&&*stop!='\t')stop++;
                        if(stop-part==5&&!strncasecmp(part,"close",5))close_after=1;
                        /* HTTP/1.0 is deliberately never retained. */
                        if(stop==part)break;part=stop;
                    }
                }
                else if(!strcasecmp(text,"Location")){if(*location)goto malformed;strcpy(location,value);}
            }
            if((has_length&&chunked)||(status==204&&(has_length||chunked)))goto malformed;
            if(status<200&&++info>4)goto malformed;
        }while(status<200);
        if(response_mode) {
            if(has_length&&!gzip) {
                snprintf(text,sizeof(text),"Content-Length: %lu",length);
                if(append_header(text))goto failed;
            }
            if(append_header("")||emit(RIPPLE_HTTP_HEADERS,response_headers,response_used))goto failed;
            out=1;
            if(status!=204&&status!=304&&status!=301&&status!=302&&status!=303&&status!=307&&status!=308&&
               (gzip?gzip_body(chunked,has_length,length):body(chunked,has_length,length)))goto failed;
            response_report();out=-1;
            /* Redirect bodies are intentionally skipped; never reuse their
             * unread bytes as the next response. Close-delimited responses,
             * HTTP/1.0 and explicit close also cannot be retained. */
            if(!worker_mode||close_after||(!has_length&&!chunked&&status!=204&&status!=304)||head!=used||
               status==301||status==302||status==303||status==307||status==308)disconnect();
            return 0;
        }
        if(status==301||status==302||status==303||status==307||status==308) {
            if(!*location||redirects++==5||redirected(url,location,next)) {
                fputs("htget: invalid, insecure or excessive redirect\n",stderr);return 1;
            }
            disconnect();strcpy(url,next);continue;
        }
        if(status<200||status>=300){fprintf(stderr,"htget: HTTP status %u\n",status);return 1;}
        if(output_file(destination))goto failed;
        if(status!=204&&(gzip?gzip_body(chunked,has_length,length):body(chunked,has_length,length)))goto failed;
        response_report();
        if(interrupted){errno=EINTR;goto failed;}
        if(out!=1) {
            if(close(out)<0){out=-1;goto failed;}out=-1;
            if(rename(temporary,destination))goto failed;temporary[0]=0;
        }
        if(strcmp(destination,"-"))fprintf(stderr,"htget: %lu octets saved to %s\n",saved,destination);
        out=-1;return 0;
    }
stale:
    if(reused&&!response_octets&&!interrupted) {
        reused=0;disconnect();tls_failed=0;head=used=request_used=0;goto retry;
    }
    goto failed;
invalid:failure="Expected a valid http:// or https:// URL";goto error;
malformed:failure="Invalid or unsupported HTTP response";goto error;
failed:
    failure=tls_failed&&*ripple_tls_error()?ripple_tls_error():strerror(errno);
error:
    fprintf(stderr,"htget: %s\n",failure);return interrupted?128+interrupted:1;
}
int ripple_http_main(int argc,char **argv) {
    int arg=1,result;const char *ca_file=NULL;
    char *data;
    while(arg<argc&&argv[arg][0]=='-'&&strcmp(argv[arg],"-")) {
        if(!strcmp(argv[arg],"--response")){response_mode=1;arg++;}
        else if(!strcmp(argv[arg],"--worker")){worker_mode=response_mode=1;arg++;}
        else if(!strcmp(argv[arg],"--body-limit")&&arg+1<argc) {
            unsigned long value;
            if(decimal(argv[arg+1],&value)||!value||value>33554432UL)break;
            body_limit=value;arg+=2;
        }
        else if(!strcmp(argv[arg],"--cacert")&&arg+1<argc){ca_file=argv[arg+1];arg+=2;}
        else if(!strcmp(argv[arg],"--header")&&arg+1<argc&&header_count<4&&allowed_header(argv[arg+1])) {
            extra_headers[header_count++]=argv[arg+1];arg+=2;
        } else break;
    }
    if(worker_mode?(argc!=arg||header_count):(argc-arg!=2||(header_count&&!response_mode)||
       (response_mode&&strcmp(argv[arg+1],"-")))) {
        fputs("usage: htget [--cacert FILE] [--response [--header LINE]...] http[s]://HOST[:PORT]/PATH FILE|-\n"
              "       htget --worker [--body-limit OCTETS] [--cacert FILE]\n"
              "--response: stdout only, no redirects, headers plus dechunked body (256 KiB default, --body-limit up to 32 MiB)\n",stderr);
        return argc==2&&!strcmp(argv[1],"--help")?0:1;
    }
    profile=getenv("RIPPLE_TLS_PROFILE")!=NULL;
    atexit(cleanup);signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGPIPE,SIG_IGN);
    if(response_mode) { response_headers=malloc(16385);if(!response_headers){perror("htget");return 1;} }
    if(!worker_mode)return download(argv[arg],argv[arg+1],ca_file);
    data=malloc(RIPPLE_HTTP_REQUEST_MAX+1);if(!data)return 1;
    while((result=request(data))>0&&!interrupted) {
        if(download(data,"-",ca_file)) {
            disconnect();buffered=0;out=-1;
            if(!failure)failure="HTTP request failed";
            if(frame(RIPPLE_HTTP_ERROR,failure,strlen(failure))){free(data);return 1;}
        } else if(frame(RIPPLE_HTTP_DONE,NULL,0)){free(data);return 1;}
    }
    free(data);
    if(interrupted)return 128+interrupted;
    if(result<0){fputs("htget: invalid or truncated worker request\n",stderr);return 1;}
    return interrupted?128+interrupted:0;
}
