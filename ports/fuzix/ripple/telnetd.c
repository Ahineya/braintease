/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "telnet-protocol.h"
#include "pty-limits.h"
extern long call(unsigned,long,long,long,long);
static volatile int stopping;
static int children[4];
static void caught(int sig){stopping=sig;signal(sig,caught);}
static void child_changed(int sig){signal(sig,child_changed);}
static void reap(void) {
    int i,status;
    for(i=0;i<4;i++)if(children[i]&&waitpid(children[i],&status,WNOHANG)==children[i])children[i]=0;
}
static int retryable(void){return errno==EAGAIN||errno==EINTR;}
static int session(int fd) {
    char masterpath[]="/dev/ptyp0",slavepath[]="/dev/ttyp0";
    unsigned char received[128],input[256],output[256];
    unsigned have=0,at=0,pending=0,sent=0,i,w=80,h=24;
    int master=-1,child,n,status,ending=0,progress;
    struct telnet protocol;
    struct winsize size;
    for(i=0;i<RIPPLE_PTY_COUNT;i++) {
        masterpath[9]=slavepath[9]='0'+i;
        master=open(masterpath,O_RDWR|O_NONBLOCK|O_NOCTTY);
        if(master>=0)break;
        if(errno!=EBUSY)return 1;
    }
    if(master<0){write(fd,"No free terminals.\r\n",20);return 1;}
    for(i=0;i<16;i++)if(i!=(unsigned)fd&&i!=(unsigned)master)close(i);
    child=fork();
    if(child<0){close(master);return 1;}
    if(!child) {
        char *args[]={"sh",NULL};
        char *env[]={"PATH=/bin","HOME=/","USER=root","TERM=ansi","PS1=fuzix# ",NULL};
        int slave;
        close(master);close(fd);
        if(setsid()<0)_exit(125);
        slave=open(slavepath,O_RDWR);
        if(slave<0)_exit(125);
        if(dup2(slave,0)<0||dup2(slave,1)<0||dup2(slave,2)<0)_exit(125);
        for(i=3;i<16;i++)close(i);
        execve("/bin/sh",args,env);_exit(127);
    }
    signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGINT,caught);signal(SIGPIPE,SIG_IGN);
    signal(SIGCHLD,child_changed);stopping=0;
    fcntl(fd,F_SETFL,O_NONBLOCK);tn_init(&protocol,1);
    tn_data(&protocol,(const unsigned char *)"\r\nFUZIX\r\n",9);
    while(!stopping) {
        progress=0;
        if(protocol.sent<protocol.used) {
            n=write(fd,protocol.wire+protocol.sent,protocol.used-protocol.sent);
            if(n>0){protocol.sent+=n;progress=1;}
            else if(n<0&&!retryable())break;
        }
        if(ending&&protocol.sent==protocol.used)break;
        if(!pending&&tn_space(&protocol)>32) {
            if(!have) {
                n=read(fd,received,sizeof(received));
                if(!n)break;
                if(n>0){have=n;at=0;progress=1;}
                else if(!retryable())break;
            }
            while(at<have) {
                if(!tn_feed(&protocol,received[at],input,&pending,sizeof(input)))break;
                ++at;
            }
            if(at==have)have=at=0;
            sent=0;
        }
        if(w!=protocol.width||h!=protocol.height) {
            size.ws_col=w=protocol.width;size.ws_row=h=protocol.height;
            size.ws_xpixel=size.ws_ypixel=0;ioctl(master,TIOCSWINSZ,&size);
        }
        if(pending) {
            n=write(master,input+sent,pending);
            if(n>0){sent+=n;pending-=n;progress=1;}
            else if(n<0&&!retryable())break;
        }
        if(!ending&&tn_space(&protocol)>=sizeof(output)*2) {
            n=read(master,output,sizeof(output));
            if(n>0){tn_data(&protocol,output,n);progress=1;}
            else if(!n)ending=1;
            else if(!retryable())break;
        }
        if(waitpid(child,&status,WNOHANG)==child) {
            child=0;
            /* Master EOF follows after its queued terminal output is drained. */
        }
        if(!progress)call(37,1,0,0,0);
    }
    close(master); /* HUP the remote foreground group and controlling session. */
    if(child) {
        kill(-child,SIGHUP);kill(-child,SIGCONT);
        for(i=0;i<50;i++) {
            if(waitpid(child,&status,WNOHANG)==child){child=0;break;}
            call(37,1,0,0,0);
        }
        if(child){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR);}
    }
    return 0;
}
int main(int argc,char **argv) {
    struct sockaddr_in address;
    struct ifreq interface;
    int listener,fd,child,i;
    unsigned port=23;
    char *end,ip[16];unsigned long number;
    if(argc>1&&!strcmp(argv[1],"--help"))goto usage;
    if(argc==3&&!strcmp(argv[1],"-p")) {
        number=strtoul(argv[2],&end,10);
        if(!*argv[2]||*end||!number||number>65535UL)goto usage;
        port=number;
    } else if(argc!=1)goto usage;
    listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0){perror("telnetd");return 1;}
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(port);
    if(bind(listener,(void *)&address,sizeof(address))||listen(listener,4)){perror("telnetd");close(listener);return 1;}
    memset(&interface,0,sizeof(interface));strcpy(interface.ifr_name,"eth0");
    strcpy(ip,"0.0.0.0");
    if(ioctl(listener,SIOCGIFADDR,&interface)==0)
        inet_ntop(AF_INET,&((struct sockaddr_in *)&interface.ifr_addr)->sin_addr,ip,16);
    printf("telnetd: listening on %s:%u\n",ip,port);fflush(stdout);
    signal(SIGCHLD,child_changed);signal(SIGTERM,caught);signal(SIGINT,caught);signal(SIGHUP,caught);signal(SIGPIPE,SIG_IGN);
    while(!stopping) {
        reap();fd=accept(listener,0,0);
        if(fd<0){if(errno==EINTR)continue;perror("telnetd: accept");break;}
        reap();for(i=0;i<4;i++)if(!children[i])break;
        if(i==4){write(fd,"Server busy.\r\n",14);close(fd);continue;}
        child=fork();
        if(!child){close(listener);i=session(fd);close(fd);_exit(i);}
        if(child>0)children[i]=child;else perror("telnetd: fork");
        close(fd);
    }
    close(listener);
    for(i=0;i<4;i++)if(children[i])kill(children[i],SIGTERM);
    for(i=0;i<4;i++)if(children[i])while(waitpid(children[i],0,0)<0&&errno==EINTR);
    return 0;
usage:fputs("usage: telnetd [-p PORT]\nStart with telnetd & for a remote shell on the attached trusted network.\n",stderr);return argc>1&&!strcmp(argv[1],"--help")?0:1;
}
