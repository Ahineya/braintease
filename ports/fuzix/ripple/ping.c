/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syscalls.h>
#include <netdb.h>
extern long call(unsigned,long,long,long,long);
static volatile int stopped;
static void stop(int sig){stopped=1;signal(sig,stop);}
static unsigned long clock_ms(void){__ktime_t t;_time(&t,1);return t.low*100UL;}
static int number(char *s,unsigned *n,unsigned max) {
    char *end;unsigned long v=strtoul(s,&end,10);
    if(!*s||*end||!v||v>max)return -1;*n=v;return 0;
}
int main(int argc,char **argv) {
    unsigned count=4,sequence=0,sent=0,received=0,id=getpid(),i,header;
    unsigned long start,deadline,elapsed;
    struct sockaddr_in peer;unsigned char packet[64],reply[1500];
    int fd,n,arg=1,matched;struct hostent *host;
    if(argc>1&&!strcmp(argv[1],"--help"))goto usage;
    if(argc>2&&!strcmp(argv[1],"-c")){if(number(argv[2],&count,1000))goto usage;arg=3;}
    if(argc!=arg+1)goto usage;
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;
    host=gethostbyname(argv[arg]);if(!host){herror(argv[arg]);return 1;}
    memcpy(&peer.sin_addr,host->h_addr_list[0],sizeof(peer.sin_addr));
    fd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);if(fd<0){perror("ping");return 1;}
    if(connect(fd,(void *)&peer,sizeof(peer))||fcntl(fd,F_SETFL,O_NONBLOCK)<0){perror("ping");close(fd);return 1;}
    signal(SIGINT,stop);signal(SIGTERM,stop);
    printf("PING %s: 32 data octets\n",argv[arg]);fflush(stdout);
    memset(packet,0,sizeof(packet));packet[0]=8;packet[4]=id>>8;packet[5]=id&255;
    for(i=8;i<40;i++)packet[i]=i;
    while(sequence<count&&!stopped) {
        ++sequence;packet[6]=sequence>>8;packet[7]=sequence&255;
        start=clock_ms();deadline=start+10000UL;
        do {
            n=send(fd,packet,40,0);
            if(n==40)break;
            if(errno!=EAGAIN&&errno!=EINTR){perror("ping");goto done;}
            call(37,1,0,0,0);
        }while(!stopped&&(long)(clock_ms()-deadline)<0);
        if(n!=40)break;
        ++sent;start=clock_ms();matched=0;
        do {
            n=read(fd,reply,sizeof(reply));
            if(n>=28) {
                header=(reply[0]&15)*4;
                if(header>=20&&n>=header+8&&reply[header]==0&&reply[header+4]==(id>>8)&&
                   reply[header+5]==(id&255)&&reply[header+6]==(sequence>>8)&&reply[header+7]==(sequence&255)) {
                    ++received;matched=1;elapsed=clock_ms()-start;
                    printf("%d octets from %s: seq=%u ttl=%u time=%lums\n",n-header,argv[arg],sequence,reply[8],elapsed);break;
                }
            } else if(n<0&&errno!=EAGAIN&&errno!=EINTR){perror("ping");goto done;}
            call(37,1,0,0,0);
        }while(!stopped&&clock_ms()-start<3000UL);
        if(!matched&&!stopped)printf("Request timeout: seq=%u\n",sequence);
        fflush(stdout);
        while(!stopped&&clock_ms()-start<1000UL)call(37,1,0,0,0);
    }
done:printf("%u packets transmitted, %u received\n",sent,received);close(fd);return received==sent&&sent?0:1;
usage:fputs("usage: ping [-c COUNT] HOST\n",stderr);return argc>1&&!strcmp(argv[1],"--help")?0:1;
}
