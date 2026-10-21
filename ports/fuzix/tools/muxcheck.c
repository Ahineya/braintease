/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include "pty-limits.h"
#include "resources.h"
static void check(int ok,const char *what){if(!ok){printf("MUXCHECK_FAILED %s\n",what);exit(1);}}
static volatile int resized;
static void resize(int sig){resized=1;signal(sig,resize);}
static void size(void){struct winsize s;check(!ioctl(0,TIOCGWINSZ,&s),"geometry");printf("MUX_GEOMETRY %u %u\n",s.ws_col,s.ws_row);fflush(stdout);}
int main(int argc,char **argv){
    int fds[32],i,pid,status;char path[]="/dev/ptyp0";fd_set reads;struct timeval wait;
    if(argc>1&&!strcmp(argv[1],"--workspace")){
        char directory[256],line[40];check(getcwd(directory,sizeof(directory))!=NULL,"workspace cwd");
        printf("WORKSPACE_DIR %s\n",directory);
        for(i=2;i<argc;i++)printf("WORKSPACE_ARG %s\n",argv[i]);
        puts("WORKSPACE_READY");fflush(stdout);
        while(fgets(line,sizeof(line),stdin))if(!strcmp(line,"quit\n"))break;
        puts("WORKSPACE_EXIT");return 0;
    }
    if(argc>1&&!strcmp(argv[1],"--burst")){
        struct termios saved,raw;unsigned count=0,j;char data[256];int n;
        check(!tcgetattr(0,&saved),"get burst termios");raw=saved;raw.c_lflag&=~(ICANON|ECHO|ISIG);raw.c_iflag&=~(ICRNL|INLCR|IXON);raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
        check(!tcsetattr(0,TCSANOW,&raw),"set burst termios");puts("MUX_BURST_READY");fflush(stdout);usleep(500000);
        while(count<6000){n=read(0,data,sizeof(data));check(n>0,"burst read");for(j=0;j<(unsigned)n;j++)check(data[j]=='x',"burst content");count+=n;}
        check(count==6000,"burst exact length");tcsetattr(0,TCSANOW,&saved);puts("MUX_BURST_OK 6000");return 0;
    }
    if(argc>1&&!strcmp(argv[1],"--keys")){
        struct termios saved,raw;unsigned count=0;char c;
        check(!tcgetattr(0,&saved),"get raw termios");raw=saved;raw.c_lflag&=~(ICANON|ECHO|ISIG);raw.c_iflag&=~(ICRNL|INLCR|IXON);raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
        check(!tcsetattr(0,TCSANOW,&raw),"set raw termios");puts("MUX_KEYS_READY");fflush(stdout);
        while(count<6){if(read(0,&c,1)==1){printf("MUX_KEY %u\n",(unsigned char)c);fflush(stdout);count++;}}
        tcsetattr(0,TCSANOW,&saved);puts("MUX_KEYS_DONE");return 0;
    }
    if(argc>1&&!strcmp(argv[1],"--hangup")){
        struct ripple_sysinfo sys;struct ripple_procinfo info;unsigned slot,parent=getppid(),attacher=0;
        int proc=open("/dev/proc",O_RDONLY);check(proc>=0&&!ioctl(proc,RIPPLE_SYSINFO,&sys),"hangup process lookup");
        for(slot=0;slot<sys.slots;slot++){memset(&info,0,sizeof(info));info.slot=slot;check(!ioctl(proc,RIPPLE_PROCINFO,&info),"parent snapshot");if(info.pid==parent)attacher=info.ppid;}
        close(proc);check(attacher>1,"attacher PID");check(!kill(attacher,SIGHUP),"outer hangup");
        puts("MUX_HANGUP_SURVIVED");fflush(stdout);for(;;)pause();
    }
    if(argc>1&&!strcmp(argv[1],"--fdclosed")){check(fcntl(20,F_GETFD,0)<0,"close-on-exec fd20");puts("MUX_FD20_CLOSED");return 0;}
    if(argc>1&&!strcmp(argv[1],"--size")){size();return 0;}
    if(argc>1&&!strcmp(argv[1],"--resize")){
        signal(SIGWINCH,resize);size();puts("MUX_RESIZE_READY");fflush(stdout);
        for(;;){pause();if(resized){resized=0;size();}}
    }
    for(i=0;i<RIPPLE_PTY_COUNT;i++){path[9]=RIPPLE_PTY_DIGIT(i);fds[i]=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK);check(fds[i]>=0,"all shared PTYs");}
    check(open("/dev/ptyp0",O_RDWR|O_NOCTTY)<0,"exclusive PTY");
    for(i=0;i<RIPPLE_PTY_COUNT;i++)close(fds[i]);
    for(i=0;i<22;i++){fds[i]=open("/dev/null",O_RDWR);check(fds[i]>=0,"high descriptor allocation");}
    check(!fcntl(20,F_SETFD,O_CLOEXEC),"set high close-on-exec");
    FD_ZERO(&reads);FD_SET(20,&reads);wait.tv_sec=0;wait.tv_usec=0;
    check(select(21,&reads,NULL,NULL,&wait)>=0,"high descriptor select");
    pid=fork();check(pid>=0,"fork");if(!pid){char *child[]={"muxcheck","--fdclosed",NULL};execvp("/bin/muxcheck",child);_exit(127);}
    check(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status),"high descriptor exec");
    for(i=0;i<22;i++)close(fds[i]);
    printf("MUXCHECK_OK %u PTYs, high descriptors, select, close-on-exec\n",RIPPLE_PTY_COUNT);return 0;
}
