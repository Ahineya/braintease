/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Session supervision only; all desktop/window policy belongs to NanoWM. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <string.h>
static volatile int stopping;
static int server,wm,launcher,terminal;
static void caught(int sig){stopping=1;signal(sig,caught);}
static int launch(char *program,char *arg,int keep){
    int pid=fork();char *args[3];
    if(pid==0){int i;setpgid(0,0);for(i=3;i<32;i++)if(i!=keep)close(i);args[0]=program;args[1]=arg;args[2]=NULL;execv(program,args);perror(program);_exit(127);}
    if(pid>0)setpgid(pid,pid);return pid;
}
static void stop(int pid){if(pid>0){kill(-pid,SIGTERM);kill(-pid,SIGCONT);}}
int main(int argc,char **argv){
    int pid,status,connected=0,i,ready[2],result=1;char fdarg[8],envarg[32],c;fd_set fds;struct timeval timeout;
    if(argc>1){puts("usage: startx\nStart the Nano-X desktop. Close its display to return here.");return 0;}
    signal(SIGTERM,caught);signal(SIGINT,caught);signal(SIGHUP,caught);
    putenv("NANOX_SESSION=1");
    if(pipe(ready)<0)return 1;
    snprintf(envarg,sizeof(envarg),"NANOX_READY_FD=%d",ready[1]);putenv(envarg);
    server=launch("/bin/nano-X","-p",ready[1]);putenv("NANOX_READY_FD=");close(ready[1]);
    FD_ZERO(&fds);FD_SET(ready[0],&fds);timeout.tv_sec=10;timeout.tv_usec=0;
    if(server>0&&select(ready[0]+1,&fds,NULL,NULL,&timeout)>0&&read(ready[0],&c,1)==1&&c=='R')connected=1;
    close(ready[0]);
    if(!connected){fputs("startx: Nano-X did not start\n",stderr);stopping=1;goto done;}
    if(pipe(ready)<0){stopping=1;goto done;}
    snprintf(fdarg,sizeof(fdarg),"%d",ready[1]);
    wm=launch("/bin/nanowm",fdarg,ready[1]);close(ready[1]);
    FD_ZERO(&fds);FD_SET(ready[0],&fds);timeout.tv_sec=10;timeout.tv_usec=0;
    connected=wm>0&&select(ready[0]+1,&fds,NULL,NULL,&timeout)>0&&read(ready[0],&c,1)==1&&c=='R';
    close(ready[0]);
    if(!connected){fputs("startx: NanoWM did not start\n",stderr);stopping=1;goto done;}
    launcher=launch("/bin/nxlaunch",NULL,-1);terminal=launch("/bin/nxterm",NULL,-1);
    if(launcher<=0||terminal<=0){stopping=1;goto done;}
    result=0;
    while(!stopping){
        pid=waitpid(-1,&status,0);
        if(pid==server){server=0;if(!WIFEXITED(status)||WEXITSTATUS(status))result=1;break;}
        if(pid==wm){wm=0;stopping=1;result=1;break;}
        if(pid==launcher){launcher=0;stopping=1;result=1;break;}
        if(pid==terminal)terminal=0;
        if(pid<0&&errno!=EINTR)break;
    }
done:
    stop(terminal);stop(launcher);stop(wm);stop(server);
    /* Bound shutdown even if a client ignores termination. */
    for(i=0;i<50;i++){
        while((pid=waitpid(-1,&status,WNOHANG))>0){if(pid==server)server=0;if(pid==wm)wm=0;if(pid==launcher)launcher=0;if(pid==terminal)terminal=0;}
        if(!server&&!wm&&!launcher&&!terminal)break;
        usleep(10000);
    }
    if(server>0)kill(-server,SIGKILL);if(wm>0)kill(-wm,SIGKILL);
    if(launcher>0)kill(-launcher,SIGKILL);if(terminal>0)kill(-terminal,SIGKILL);
    while(waitpid(-1,&status,0)>0||errno==EINTR){}
    return result;
}
