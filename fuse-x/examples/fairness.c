/* SPDX-License-Identifier: GPL-2.0-only */
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <string.h>
static volatile unsigned long counter;
static volatile int caught;
static jmp_buf escape;
static void alarm_handler(int sig) { (void)sig; caught=1; }
static void jump_handler(int sig) { (void)sig; longjmp(escape,7); }
static void busy(void) { for(;;) counter=counter*1664525UL+1013904223UL; }
int main(int argc,char **argv) {
    int child,status;
    unsigned long a=3,b=7;
    if(argc>1 && !strcmp(argv[1],"forever")) busy();
    if(argc>1 && !strcmp(argv[1],"orphan")) {
        child=fork();
        if(child<0)return 10;
        if(!child) {sleep(1);_exit(0);}
        return 0;
    }
    signal(SIGALRM,alarm_handler); alarm(1);
    while(!caught) {
        unsigned long old=a;
        a=b; b=old+b;
        counter=a^b;
    }
    if(!a && !b) return 1;
    if(setjmp(escape)!=7) {
        signal(SIGALRM,jump_handler); alarm(1); busy();
        return 2;
    }
    child=fork();
    if(child<0)return 3;
    if(!child)busy();
    sleep(1);
    if(kill(child,SIGSTOP) || waitpid(child,&status,WUNTRACED)!=child || !WIFSTOPPED(status))return 4;
    if(kill(child,SIGCONT))return 5;
    sleep(1);
    if(kill(child,SIGTERM) || waitpid(child,&status,0)!=child || !WIFSIGNALED(status))return 6;
    puts("busy loops: timers, signal handlers, longjmp, stop/continue and termination passed");
    return 0;
}
