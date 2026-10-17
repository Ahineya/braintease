/* Real loaded-image coverage of nonlocal jumps and FUZIX signal delivery. */
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
static jmp_buf outer, inner;
static volatile int seen, calls;
static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr,"CONTROL CHECK FAILED: %s (errno %d)\n",what,errno); exit(1); }
}
static void unwind(int depth, int value) {
    volatile int local=depth;
    if(depth) unwind(depth-1,value);
    require(local==depth,"recursive locals");
    longjmp(outer,value);
}
static void caught(int sig) {
    seen=sig; ++calls;
    require(getpid()>1,"handler can enter kernel");
    require(signal(sig,caught)!=SIG_ERR,"handler reinstalls itself");
}
static void jumped(int sig) { seen=sig; longjmp(outer,42); }
int main(void) {
    volatile int state=0;
    int value,child,status;
    value=setjmp(outer);
    if(!value) { state=9; unwind(5,0); }
    require(value==1 && state==9,"zero longjmp becomes one and locals remain changed");
    value=setjmp(outer);
    if(!value) unwind(4,-7);
    require(value==-7,"signed jump value");
    value=setjmp(outer);
    if(!value) {
        int nested=setjmp(inner);
        if(!nested) longjmp(inner,3);
        require(nested==3,"nested jump buffer");
        longjmp(outer,5);
    }
    require(value==5,"outer buffer after inner jump");
    value=setjmp(outer);
    if(!value) {
        child=fork();require(child>=0,"fork with live jump");
        if(!child) longjmp(outer,17);
        require(waitpid(child,&status,0)==child && status==(17<<8),"child inherited jump");
    } else { require(value==17,"child jump value");_exit(17); }
    require(signal(SIGUSR1,caught)!=SIG_ERR,"install native handler");
    require(raise(SIGUSR1)==0 && seen==SIGUSR1 && calls==1,"first handler dispatch");
    require(raise(SIGUSR1)==0 && calls==2,"reinstalled handler");
    value=setjmp(outer);
    if(!value) {
        require(signal(SIGUSR2,jumped)!=SIG_ERR,"install jumping handler");
        raise(SIGUSR2); require(0,"handler longjmp returned");
    }
    require(value==42 && seen==SIGUSR2,"longjmp out of handler");
    require(getpid()>1,"kernel remains usable after handler jump");
    require(signal(SIGALRM,caught)!=SIG_ERR,"alarm handler");
    alarm(1);errno=0;
    require(pause()<0 && errno==EINTR && seen==SIGALRM,"alarm interrupts blocking syscall");
    require(signal(SIGINT,(sighandler_t)0x99999999UL)==SIG_ERR && errno==EFAULT,"invalid handler rejected");
    puts("Fuse-X: continuation and signal checks passed");
    return 0;
}
