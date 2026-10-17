/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "telnet-protocol.h"
static struct termios saved;
static int raw;
static volatile int stopped;
static void restore(void){if(raw)tcsetattr(0,TCSANOW,&saved);}
static void stop(int sig){stopped=1;signal(sig,stop);}
int main(int argc,char **argv) {
    struct sockaddr_in peer;struct termios terminal;struct winsize size;
    struct telnet protocol;
    unsigned char fromnet[128],input[128],output[256];
    unsigned have=0,at=0,used=0,i,port=23;
    unsigned long number;char *end;int fd,n,quit=0,result=0;struct hostent *host;
    if(argc>1&&!strcmp(argv[1],"--help"))goto usage;
    if(argc<2||argc>3)goto usage;
    if(argc==3){number=strtoul(argv[2],&end,10);if(!*argv[2]||*end||!number||number>65535UL)goto usage;port=number;}
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(port);
    host=gethostbyname(argv[1]);if(!host){herror(argv[1]);return 1;}
    memcpy(&peer.sin_addr,host->h_addr_list[0],sizeof(peer.sin_addr));
    fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){perror("telnet");return 1;}
    printf("Connecting to %s:%u...\n",argv[1],port);fflush(stdout);
    if(connect(fd,(void *)&peer,sizeof(peer))<0){perror("telnet");close(fd);return 1;}
    puts("Connected. Ctrl+^ closes the connection.");fflush(stdout);
    if(!isatty(0)||tcgetattr(0,&saved)<0){fputs("telnet: interactive terminal required\n",stderr);close(fd);return 1;}
    terminal=saved;terminal.c_lflag&=~(ICANON|ECHO|ISIG);
    terminal.c_iflag&=~(ICRNL|INLCR|IXON);terminal.c_oflag&=~OPOST;
    terminal.c_cc[VMIN]=0;terminal.c_cc[VTIME]=1;
    if(tcsetattr(0,TCSANOW,&terminal)<0){perror("telnet");close(fd);return 1;}
    raw=1;atexit(restore);tn_init(&protocol,0);
    if(ioctl(0,TIOCGWINSZ,&size)==0&&size.ws_col&&size.ws_row){protocol.width=size.ws_col;protocol.height=size.ws_row;}
    fcntl(fd,F_SETFL,O_NONBLOCK);signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,stop);signal(SIGHUP,stop);signal(SIGINT,stop);
    while(!stopped&&!quit) {
        if(protocol.sent<protocol.used) {
            n=write(fd,protocol.wire+protocol.sent,protocol.used-protocol.sent);
            if(n>0)protocol.sent+=n;
            else if(n<0&&errno!=EINTR&&errno!=EAGAIN){result=1;break;}
        }
        if(tn_space(&protocol)>=400) {
            if(!have) {
                n=read(fd,fromnet,sizeof(fromnet));
                if(!n)break;
                if(n>0){have=n;at=0;}
                else if(errno!=EINTR&&errno!=EAGAIN){result=1;break;}
            }
            used=0;
            while(at<have){if(!tn_feed(&protocol,fromnet[at],output,&used,sizeof(output)))break;++at;}
            if(at==have)at=have=0;
            for(i=0;i<used;) {
                n=write(1,output+i,used-i);if(n<=0){quit=1;break;}i+=n;
            }
        }
        if(tn_space(&protocol)>=sizeof(input)*2) {
            n=read(0,input,sizeof(input));
            if(n>0) {
                for(i=0;i<(unsigned)n;i++)if(input[i]==30){quit=1;n=i;break;}
                tn_data(&protocol,input,n);
            } else if(n<0&&errno!=EINTR&&errno!=EAGAIN){result=1;break;}
        }
    }
    close(fd);restore();raw=0;puts("\nConnection closed.");return result;
usage:fputs("usage: telnet HOST [PORT]\n",stderr);return argc>1&&!strcmp(argv[1],"--help")?0:1;
}
