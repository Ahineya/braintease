/* Real guest cache policy: contents, identity checks, fork and live references. */
#include <ripple_tls.h>
#include <netutil.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do { if(!(x)){printf("trustreuse failed line %u: %s errno %d\n",__LINE__,ripple_tls_error(),errno);return 1;} } while(0)
static void copy(const char *source) {
    char block[512];int in=open(source,O_RDONLY),out=open("/trust",O_WRONLY|O_TRUNC|O_CREAT,0600);ssize_t n;
    if(in<0||out<0)exit(2);
    while((n=read(in,block,sizeof(block)))>0)if(nu_write(out,block,n,30000UL))exit(2);
    if(n<0)exit(2);close(in);close(out);
}
static RippleTLS *connection(int *fd,const char *host) {
    struct hostent *h=gethostbyname("tls.test");struct sockaddr_in peer;
    if(!h)return NULL;
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(443);
    memcpy(&peer.sin_addr,h->h_addr_list[0],sizeof(peer.sin_addr));
    *fd=socket(AF_INET,SOCK_STREAM,0);
    if(*fd<0||nu_connect(*fd,(void *)&peer,sizeof(peer),30000UL))return NULL;
    if(fcntl(*fd,F_SETFL,0)<0)return NULL;
    return ripple_tls_open(*fd,host,"/trust",120000UL,NULL);
}
static int response(RippleTLS *t) {
    char block[1024];ssize_t n;
    const char *request="GET /binary HTTP/1.1\r\nHost: tls.test\r\nConnection: close\r\n\r\n";
    if(ripple_tls_write(t,request,strlen(request),30000UL))return -1;
    while((n=ripple_tls_read(t,block,sizeof(block),30000UL))>0){}
    return n<0?-1:0;
}
static int fetch(const char *host) {
    int fd=-1,result=-1;RippleTLS *t=connection(&fd,host);
    if(t){result=response(t);ripple_tls_close(t);}if(fd>=0)close(fd);return result;
}
int main(void) {
    int fd=-1,pid,status;RippleTLS *live;
    copy("/good");puts("CACHE first");C(fetch("tls.test")==0);
    puts("CACHE repeat");C(fetch("tls.test")==0);
    puts("CACHE hostname");C(fetch("wrong.test")<0);
    puts("CACHE live");live=connection(&fd,"tls.test");C(live!=NULL);
    copy("/other");puts("CACHE replaced same size");C(fetch("tls.test")<0);
    /* Replacing the cache does not change an already verified live session. */
    C(response(live)==0);ripple_tls_close(live);close(fd);
    copy("/good");puts("CACHE restored");C(fetch("tls.test")==0);
    C(unlink("/trust")==0);puts("CACHE missing");C(fetch("tls.test")<0);
    copy("/good");puts("CACHE fork");live=connection(&fd,"tls.test");C(live!=NULL);
    pid=fork();C(pid>=0);
    if(!pid) {
        if(ripple_tls_write(live,"x",1,30000UL)!=-1||errno!=EIO)_exit(3);
        ripple_tls_close(live);close(fd);
        if(fetch("tls.test"))_exit(4);
        puts("CACHE child fresh");exit(0);
    }
    C(waitpid(pid,&status,0)==pid&&status==0);
    C(fcntl(fd,F_GETFL,0)&O_NONBLOCK);
    C(response(live)==0);ripple_tls_close(live);close(fd);
    puts("CACHE parent repeat");C(fetch("tls.test")==0);
    puts("trustreuse passed");return 0;
}
