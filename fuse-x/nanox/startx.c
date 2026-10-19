/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ensure the native Nano-X service, then open the native graphical terminal.
 * No guest window manager or desktop compositor is started in hosted mode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <errno.h>
#include "session.h"
static int connected(void) {
    struct sockaddr_un a;int fd,result;
    memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strcpy(a.sun_path,"/tmp/.nano-X16");
    fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return 0;
    result=connect(fd,(struct sockaddr *)&a,sizeof(a));close(fd);return result==0;
}
static int launch(char *program,char *arg,int keep) {
    int pid=fork();char *args[3];
    if(!pid){int i;if(setsid()<0)_exit(125);nxh_detach_terminal();for(i=3;i<32;i++)if(i!=keep)close(i);
        args[0]=program;args[1]=arg;args[2]=NULL;execv(program,args);perror(program);_exit(127);}
    return pid;
}
int main(int argc,char **argv) {
    int ready[2],server,ok;char fdarg[32],c;fd_set fds;struct timeval timeout;
    if(argc>1){puts("usage: startx\nOpen gterm with Nano-X available for graphical applications. The service remains available after its windows close.");return strcmp(argv[1],"--help")?2:0;}
    putenv("NANOX_SESSION=1");
    if(!connected()) {
        if(pipe(ready)<0)return 1;
        snprintf(fdarg,sizeof(fdarg),"NANOX_READY_FD=%d",ready[1]);putenv(fdarg);
        server=launch("/bin/nano-X","-p",ready[1]);putenv("NANOX_READY_FD=");close(ready[1]);
        FD_ZERO(&fds);FD_SET(ready[0],&fds);timeout.tv_sec=10;timeout.tv_usec=0;
        ok=server>0&&select(ready[0]+1,&fds,NULL,NULL,&timeout)>0&&read(ready[0],&c,1)==1&&c=='R';close(ready[0]);
        if(!ok){if(server>0){kill(server,SIGTERM);waitpid(server,NULL,0);}fputs("startx: Nano-X did not start\n",stderr);return 1;}
    }
    if(launch("/bin/gterm",NULL,-1)<0)return 1;
    puts("Native graphical terminal started");return 0;
}
