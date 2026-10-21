/* Narrow HTTP transport diagnostic. TLS clients retain their separate port. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netutil.h>
int main(int argc,char **argv) {
    struct sockaddr_in peer;
    static unsigned char response[8192];
    const char *request="GET /probe HTTP/1.0\r\nHost: riplix-test\r\nConnection: close\r\n\r\n";
    unsigned n=0,header=0,i;unsigned long port;char *end;
    int fd,out;ssize_t got;
    if(argc!=4){fputs("usage: netcheck IPv4 PORT OUTPUT\n",stderr);return 2;}
    port=strtoul(argv[2],&end,10);
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(port);
    if(!*argv[2]||*end||!port||port>65535UL||inet_pton(AF_INET,argv[1],&peer.sin_addr)!=1)return 2;
    fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0||nu_connect(fd,(void *)&peer,sizeof(peer),15000UL)||nu_write(fd,request,strlen(request),15000UL)) {
        perror("netcheck: connect/write");if(fd>=0)close(fd);return 1;
    }
    for(;;) {
        if(n==sizeof(response)){fputs("netcheck: response exceeds bound\n",stderr);close(fd);return 1;}
        got=nu_read(fd,response+n,sizeof(response)-n,15000UL);
        if(got<0){perror("netcheck: read");close(fd);return 1;}
        if(!got)break;n+=got;
    }
    close(fd);
    for(i=3;i<n;i++)if(response[i-3]==13&&response[i-2]==10&&response[i-1]==13&&response[i]==10){header=i+1;break;}
    if(n<12||memcmp(response,"HTTP/1.",7)||response[9]!='2'||response[10]!='0'||response[11]!='0'||!header) {
        fputs("netcheck: invalid HTTP response\n",stderr);return 1;
    }
    out=open(argv[3],O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(out<0){perror("netcheck: output");return 1;}
    i=header;
    while(i<n){got=write(out,response+i,n-i);if(got<=0){perror("netcheck: file write");close(out);return 1;}i+=got;}
    if(close(out)<0)return 1;
    printf("NETWORK_HTTP_OK %u octets\n",n-header);return 0;
}
