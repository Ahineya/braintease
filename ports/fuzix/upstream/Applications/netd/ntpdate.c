/* A Cheesey NTP client to set system time.
 * (C) 2017 Brett M. Gordon, GPL2 under Fuzix.
 * Ripple adaptation: explicit octets, reply correlation, monotonic timeouts,
 * UTC clock updates and checked options. */
#include <netutil.h>
#include <netdb.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <arpa/inet.h>
static void put32(unsigned char *p,unsigned long v){p[0]=v>>24;p[1]=(v>>16)&255;p[2]=(v>>8)&255;p[3]=v&255;}
int main(int argc,char **argv) {
    struct sockaddr_in peer;struct hostent *h;unsigned char query[48],reply[512];
    unsigned long port=123,start,stamp;long hours;time_t utc,display;int option,set=0,offset=0,fd,n,attempt;char *end;
    if(argc==2&&!strcmp(argv[1],"--help"))goto usage;
    while((option=getopt(argc,argv,"p:o:ds"))!=-1) {
        if(option=='p'){port=strtoul(optarg,&end,10);if(!*optarg||*end||!port||port>65535UL)goto usage;}
        else if(option=='o'){hours=strtol(optarg,&end,10);if(!*optarg||*end||hours<-12||hours>14)goto usage;offset=hours;}
        else if(option=='s')set=1;else if(option!='d')goto usage;
    }
    if(argc-optind!=1)goto usage;
    h=gethostbyname(argv[optind]);if(!h){herror(argv[optind]);return 1;}
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(port);
    memcpy(&peer.sin_addr,h->h_addr_list[0],sizeof(peer.sin_addr));
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("ntpdate");return 1;}
    if(connect(fd,(void *)&peer,sizeof(peer))||fcntl(fd,F_SETFL,O_NONBLOCK)<0)goto failed;
    for(attempt=0;attempt<3;attempt++) {
        memset(query,0,sizeof(query));query[0]=0x23;
        put32(query+40,time(0)+2208988800UL);
        put32(query+44,nu_now()^((unsigned long)getpid()<<16)^attempt);
        start=nu_now();
        if(nu_write(fd,query,sizeof(query),2000UL)) {if(errno==EINTR)goto failed;continue;}
        while(!nu_expired(start,2000UL)) {
            n=read(fd,reply,sizeof(reply));
            if(n<0){if(errno!=EAGAIN||nu_wait())goto failed;continue;}
            if(n<48||(reply[0]&7)!=4||((reply[0]>>3)&7)<3||((reply[0]>>3)&7)>4||
               (reply[0]>>6)==3||!reply[1]||reply[1]>15||memcmp(reply+24,query+40,8))continue;
            stamp=ripple_dns_u32(reply+40);if(!stamp)continue;
            utc=stamp-2208988800UL;
            close(fd);
            if(set&&stime(&utc)<0){perror("ntpdate: stime");return 1;}
            display=utc+(long)offset*3600L;
            printf("%s",ctime(&display));return 0;
        }
    }
    errno=ETIMEDOUT;
 failed:perror("ntpdate");close(fd);return 1;
 usage:fputs("usage: ntpdate [-d] [-s] [-p PORT] [-o DISPLAY_HOURS] SERVER\n",stderr);return argc==2&&!strcmp(argv[1],"--help")?0:1;
}
