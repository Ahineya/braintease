/* Independent PTY session and queue backpressure through the hosted kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
extern long call(unsigned,long,long,long,long);
static void require(int ok,const char *what) {
    if(!ok) {fprintf(stderr,"PTY CHECK FAILED: %s (errno %d)\n",what,errno);exit(1);}
}
int main(void) {
    int master,child,status,n,fd,i;
    unsigned count=0,tries=0;
    char block[800],reply[128],input[16],tail[128];
    master=open("/dev/ptyp0",O_RDWR|O_NOCTTY|O_NONBLOCK);
    require(master>=0,"open master");
    child=fork();require(child>=0,"fork");
    if(!child) {
        close(master);
        require(setsid()>0,"new session");
        fd=open("/dev/ttyp0",O_RDWR);require(fd>=0,"open slave");
        require(dup2(fd,0)==0 && dup2(fd,1)==1 && dup2(fd,2)==2,"slave descriptors");
        if(fd>2) close(fd);
        require(tcgetpgrp(0)==getpgrp(),"independent foreground group");
        memset(block,'x',sizeof(block));
        require(write(1,block,sizeof(block))==sizeof(block),"large slave output");
        require(read(0,input,sizeof(input))==6 && !memcmp(input,"hello\n",6),"canonical slave input");
        puts("PTY-read-ok");fflush(stdout);_exit(0);
    }
    memset(tail,0,sizeof(tail));
    while(count<800 && tries++<100) {
        n=read(master,reply,sizeof(reply));
        if(n<0) {require(errno==EAGAIN || errno==EINTR,"master read");call(37,1,0,0,0);continue;}
        require(n>0,"premature slave EOF");
        for(i=0;i<n;i++) require(reply[i]=='x',"output bytes");
        count+=n;
    }
    require(count==800,"PTY output queue backpressure");
    require(write(master,"hello\n",6)==6,"send slave input");
    count=0;tries=0;
    while(tries++<100) {
        n=read(master,reply,sizeof(reply)-1);
        if(n<0) {require(errno==EAGAIN || errno==EINTR,"reply read");call(37,1,0,0,0);continue;}
        if(!n) break;
        require(count+n<sizeof(tail),"reply length");
        memcpy(tail+count,reply,n);count+=n;tail[count]=0;
        if(strstr(tail,"PTY-read-ok")) break;
    }
    require(strstr(tail,"PTY-read-ok")!=NULL,"round trip");
    require(waitpid(child,&status,0)==child && status==0,"PTY child exit");
    require(read(master,reply,sizeof(reply))==0,"slave EOF");close(master);
    master=open("/dev/ptyp0",O_RDWR|O_NOCTTY|O_NONBLOCK);
    require(master>=0,"reuse PTY slot");close(master);
    puts("Fuse-X: PTY session checks passed");return 0;
}
