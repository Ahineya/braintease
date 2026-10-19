/* Optional kernel benchmark: syscalls, pipes, fork/wait and blocking IPC. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
static volatile unsigned sink;
static char data[512];
static void check(int ok) { if(!ok){perror("probe");exit(1);} }
static void stamp(struct timespec *t) { check(!clock_gettime(CLOCK_MONOTONIC,t)); }
static void show(const char *name,unsigned count,struct timespec *a,struct timespec *b) {
    unsigned long us=(b->tv_sec-a->tv_sec)*1000000UL+(b->tv_nsec-a->tv_nsec)/1000L;
    printf("MEASURE %s %u %lu\n",name,count,us);fflush(stdout);
}
static void transfers(int fd,int writing,unsigned n) {
    unsigned done=0;int amount;
    while(done<n) {
        amount=writing?write(fd,data+done,n-done):read(fd,data+done,n-done);
        check(amount>0);done+=amount;
    }
}
static void pipe_lifetime(void) {
    int fds[2];unsigned i;char value;struct timespec a,b;
    stamp(&a);
    for(i=0;i<200;++i) {
        check(!pipe(fds));
        check(write(fds[1],"p",1)==1&&read(fds[0],&value,1)==1&&value=='p');
        check(!close(fds[0])&&!close(fds[1]));
    }
    stamp(&b);show("pipe_lifetime",200,&a,&b);
}
static void self_pipe(unsigned size) {
    int fds[2];unsigned i;struct timespec a,b;
    check(!pipe(fds));
    stamp(&a);
    for(i=0;i<200;++i){transfers(fds[1],1,size);transfers(fds[0],0,size);}
    stamp(&b);show(size==1?"pipe_self_1":"pipe_self_512",200,&a,&b);
    check(!close(fds[0])&&!close(fds[1]));
}
static void pingpong(unsigned size) {
    int req[2],reply[2],pid,status;unsigned i;struct timespec a,b;
    check(!pipe(req)&&!pipe(reply));pid=fork();check(pid>=0);
    if(!pid) {
        close(req[1]);close(reply[0]);
        for(i=0;i<210;++i){transfers(req[0],0,size);transfers(reply[1],1,size);}
        _exit(0);
    }
    close(req[0]);close(reply[1]);
    for(i=0;i<10;++i){transfers(req[1],1,size);transfers(reply[0],0,size);}
    stamp(&a);
    for(i=0;i<200;++i){transfers(req[1],1,size);transfers(reply[0],0,size);}
    stamp(&b);show(size==1?"pingpong_1":"pingpong_512",200,&a,&b);
    check(waitpid(pid,&status,0)==pid&&!status);close(req[1]);close(reply[0]);
}
int main(void) {
    unsigned i;int pid,status;struct timespec a,b,t;
    stamp(&a);for(i=0;i<1000;++i)sink+=i;stamp(&b);show("loop",1000,&a,&b);
    stamp(&a);for(i=0;i<1000;++i)sink+=getpid();stamp(&b);show("getpid",1000,&a,&b);
    stamp(&a);for(i=0;i<1000;++i)stamp(&t);stamp(&b);show("clock",1000,&a,&b);
    pipe_lifetime();self_pipe(1);self_pipe(512);
    stamp(&a);
    for(i=0;i<10;++i){pid=fork();check(pid>=0);if(!pid)_exit(0);check(waitpid(pid,&status,0)==pid&&!status);}
    stamp(&b);show("fork_exit_wait",10,&a,&b);
    pingpong(1);pingpong(512);puts("PROBE_OK");return 0;
}
