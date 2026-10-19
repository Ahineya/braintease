/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Queue arithmetic plus real guest timing, shared by native and RVM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include "../ripple/deadline.c"
#include "../ripple/clock-abi.h"
extern long call(unsigned, long, long, long, long);

static unsigned checks;
static volatile int caught;
static volatile unsigned long spin;
static void fail(unsigned line) { printf("FINE_FAILED line %u errno %d\n",line,errno);exit(1); }
#define C(x) do { ++checks; if (!(x)) fail(__LINE__); } while (0)
static void handler(int sig) { ++caught; signal(sig, handler); }
static unsigned long millis(void) {
    struct timespec t;
    C(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (unsigned long)t.tv_sec * 1000UL + t.tv_nsec / 1000000L;
}
static void waited(int pid) {
    int status;
    C(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static unsigned long poll_low(struct ripple_deadline_queue *q,const struct ripple_deadline_time *t) {
    struct ripple_deadline_ready ready;
    ripple_deadline_poll(q,t,&ready);C(ready.bits[1]==0);return ready.bits[0];
}
static void queue(void) {
    struct ripple_deadline_queue q;
    struct ripple_deadline_time t = {10, 0, 999999000UL}, at;
    unsigned i;
    memset(&q, 0, sizeof(q));
    C(poll_low(&q, &t) == 0);
    C(ripple_deadline_arm(&q, 0, &t, 0, 1) == 0);
    at = q.slots[0].at;
    C(at.seconds_low == 11 && at.seconds_high == 0 && at.nanoseconds == 0);
    C(poll_low(&q, &t) == 0 && q.slots[0].state == RIPPLE_DEADLINE_ARMED);
    C(poll_low(&q, &at) == 1 && !q.head && q.slots[0].state == RIPPLE_DEADLINE_EXPIRED);
    C(poll_low(&q, &at) == 0);
    t.seconds_low = 0xffffffffUL; t.seconds_high = 7;
    C(ripple_deadline_arm(&q, 31, &t, 0, 1) == 0);
    at = q.slots[31].at;
    C(at.seconds_low == 0 && at.seconds_high == 8 && at.nanoseconds == 0);
    C(poll_low(&q, &t) == 0);
    C(poll_low(&q, &at) == 0x80000000UL);
    t.seconds_low = 100; t.seconds_high = 1; t.nanoseconds = 0;
    C(ripple_deadline_arm(&q, 2, &t, 0x7fffffffUL, 999999) == 0);
    at = q.slots[2].at;
    C(at.seconds_low == 0x80000063UL && at.seconds_high == 1 && at.nanoseconds == 999999000UL);
    C(poll_low(&q, &t) == 0);
    ripple_deadline_cancel(&q, 2); C(!q.head && q.slots[2].state == RIPPLE_DEADLINE_NONE);
    for (i=0; i<32; ++i) C(ripple_deadline_arm(&q, i, &t, 0, (32-i)*1000UL) == 0);
    at=t;at.nanoseconds=1000000UL;
    C(poll_low(&q,&at)==0x80000000UL);
    ripple_deadline_cancel(&q,30); /* current head */
    ripple_deadline_cancel(&q,16); /* middle */
    ripple_deadline_cancel(&q,0);  /* tail */
    ripple_deadline_cancel(&q,16); /* idempotent */
    at.nanoseconds=32000000UL;
    C(poll_low(&q,&at)==0x3ffefffeUL && !q.head);
    C(ripple_deadline_arm(&q,16,&t,0,1000)==0);
    C(ripple_deadline_arm(&q,16,&t,0,2000)==0); /* replace, not duplicate */
    at.nanoseconds=1000000UL;C(poll_low(&q,&at)==0);
    at.nanoseconds=2000000UL;C(poll_low(&q,&at)==0x10000UL && !q.head);
    C(ripple_deadline_arm(&q,1,&t,0,1000)==0 && ripple_deadline_arm(&q,4,&t,0,1000)==0);
    at.nanoseconds=1000000UL;C(poll_low(&q,&at)==18 && !q.head);
    C(ripple_deadline_arm(&q,3,&t,0,1000)==0 && ripple_deadline_arm(&q,31,&t,1,0)==0);
    C(poll_low(&q,NULL)==0x80000008UL && !q.head);
    C(q.slots[3].state==RIPPLE_DEADLINE_ERROR && q.slots[31].state==RIPPLE_DEADLINE_ERROR);
    ripple_deadline_cancel(&q,3);C(q.slots[3].state==RIPPLE_DEADLINE_NONE);
    C(ripple_deadline_arm(&q,RIPPLE_DEADLINE_SLOTS,&t,0,0)==-1);
    {
        struct ripple_deadline_ready ready;
        for(i=0;i<RIPPLE_DEADLINE_SLOTS;i++)C(ripple_deadline_arm(&q,i,&t,0,1000)==0);
        at=t;at.nanoseconds=1000000UL;ripple_deadline_poll(&q,&at,&ready);
        C(ready.bits[0]==0xffffffffUL && ready.bits[1]==0xffffffffUL && !q.head);
        C(q.slots[32].state==RIPPLE_DEADLINE_EXPIRED && q.slots[63].state==RIPPLE_DEADLINE_EXPIRED);
        C(ripple_deadline_arm(&q,32,&t,1,0)==0 && ripple_deadline_arm(&q,63,&t,1,0)==0);
        ripple_deadline_poll(&q,NULL,&ready);
        C(ready.bits[0]==0 && ready.bits[1]==0x80000001UL);
        C(q.slots[32].state==RIPPLE_DEADLINE_ERROR && q.slots[63].state==RIPPLE_DEADLINE_ERROR);
    }
    C(ripple_deadline_arm(&q,0,&t,0,1000000UL)==-1 && !q.head);
    t.seconds_low=t.seconds_high=0xffffffffUL;t.nanoseconds=999999000UL;
    C(ripple_deadline_arm(&q,0,&t,0,1)==-1 && !q.head);
    puts("finecheck: synthetic clock boundaries, ordering, cancellation, errors and 64 slots passed");
}
static void clocks(void) {
    struct timespec t;
    __ktime_t legacy;
    struct { unsigned before; struct ripple_clock_sample sample; unsigned after; } c;
    c.before=0x1234;c.after=0x4321;
    C(sizeof(c.sample)==8);
    C(call(27,(long)&c.sample,RIPPLE_TIME_MONOTONIC,0,0)==0);
    C(c.before==0x1234 && c.after==0x4321 && c.sample.nanoseconds<1000000000UL);
    C(c.sample.resolution_ns>0 && c.sample.resolution_ns<=1000000UL);
    /* Bank 24 is now valid heap space; bank 240 is outside both user maps. */
    C(call(27,0xf00000UL,RIPPLE_TIME_MONOTONIC,0,0)==-1 && errno==EFAULT);
    C(clock_getres(CLOCK_MONOTONIC,&t)==0 && t.tv_sec==0 && t.tv_nsec>0 && t.tv_nsec<=1000000L);
    C(clock_getres(CLOCK_REALTIME,&t)==0 && t.tv_sec==1 && t.tv_nsec==0);
    C(clock_getres(CLOCK_MONOTONIC,NULL)==0);
    t.tv_sec=99;t.tv_nsec=88;
    C(clock_getres(9,&t)==-1 && errno==EINVAL && t.tv_sec==99 && t.tv_nsec==88);
    C(clock_gettime(9,&t)==-1 && errno==EINVAL && t.tv_sec==99 && t.tv_nsec==88);
    C(clock_gettime(CLOCK_MONOTONIC,NULL)==-1 && errno==EFAULT);
    C(_time(&legacy,1)==0 && legacy.high==0);
    puts("finecheck: monotonic clock/resolution ABI, guards and legacy clock passed");
}
static void timing(void) {
    static const unsigned usecs[] = {1000, 5000, 10000, 20000, 50000};
    struct timeval t;
    unsigned i, round;
    unsigned long start, elapsed, totals[5] = {0};
    /* Aggregate ceilings distinguish fine deadlines from 100 ms rounding
     * without requiring every host scheduling event to meet a hard deadline. */
    for (round=0; round<4; ++round) for(i=0;i<5;++i) {
        t.tv_sec=0;t.tv_usec=usecs[i];start=millis();
        C(select(0,NULL,NULL,NULL,&t)==0);
        elapsed=millis()-start;
        C(elapsed >= usecs[i]/1000 && elapsed < 2000);
        C(t.tv_sec==0 && t.tv_usec==usecs[i]);totals[i]+=elapsed;
    }
    for(i=0;i<5;++i) {
        printf("FINE_WAIT %u us: %lu ms average\n",usecs[i],totals[i]/4);
        C(totals[i] <= 4UL*(usecs[i]/1000+40));
    }
    start=millis();C(usleep(50000UL)==0);elapsed=millis()-start;
    printf("FINE_USLEEP_50 %lu ms\n",elapsed);C(elapsed>=50 && elapsed<100);
    puts("finecheck: 1/5/10/20/50 ms select and usleep passed");
}
static void lifecycle(void) {
    int p[2], pid, i, status;
    struct timeval t;
    fd_set r;
    unsigned long start, elapsed;
    unsigned seen=0;
    char byte;
    signal(SIGUSR1,handler);
    for (i=0;i<4;++i) {
        C(pipe(p)==0);pid=fork();C(pid>=0);
        if(!pid){usleep(20000UL);kill(getppid(),SIGUSR1);_exit(0);}
        t.tv_sec=0x7fffffffUL;t.tv_usec=999999;FD_ZERO(&r);FD_SET(p[0],&r);
        C(select(p[0]+1,&r,NULL,NULL,&t)==-1 && errno==EINTR);
        waited(pid);close(p[0]);close(p[1]);
        C(usleep(5000UL)==0); /* stale long timer must have been removed */
    }
    C(caught==4);
    /* Spurious wakeups retain an absolute deadline; repeated SIGCONT cannot
     * extend it. Ignore SIGCONT, preserving the kernel's normal wake behavior. */
    signal(SIGCONT,SIG_IGN);
    pid=fork();C(pid>=0);
    if(!pid){for(i=0;i<8;++i){usleep(10000UL);kill(getppid(),SIGCONT);}_exit(0);}
    t.tv_sec=0;t.tv_usec=100000;start=millis();C(select(0,NULL,NULL,NULL,&t)==0);
    elapsed=millis()-start;C(elapsed>=100 && elapsed<200);waited(pid);
    /* A caught signal must interrupt usleep too, without replacing an alarm. */
    pid=fork();C(pid>=0);
    if(!pid){usleep(20000UL);kill(getppid(),SIGUSR1);_exit(0);}
    C(usleep(1000000UL)==-1 && errno==EINTR);waited(pid);C(caught==5);
    signal(SIGALRM,handler);alarm(1);t.tv_sec=2;t.tv_usec=0;
    C(select(0,NULL,NULL,NULL,&t)==-1 && errno==EINTR && caught==6);alarm(0);
    /* Timeouts expire independently when several processes are asleep. */
    C(pipe(p)==0);
    for(i=0;i<3;++i){pid=fork();C(pid>=0);if(!pid){close(p[0]);start=millis();C(usleep((i+1)*20000UL)==0);C(millis()-start>=(i+1)*20UL);byte='0'+i;_exit(write(p[1],&byte,1)==1?0:8);}}
    close(p[1]);
    for(i=0;i<3;++i){t.tv_sec=1;t.tv_usec=0;FD_ZERO(&r);FD_SET(p[0],&r);C(select(p[0]+1,&r,NULL,NULL,&t)==1);C(read(p[0],&byte,1)==1 && byte>='0' && byte<='2');C(!(seen & (1U << (byte-'0'))));seen|=1U << (byte-'0');}
    close(p[0]);for(i=0;i<3;++i)C(waitpid(-1,&status,0)>0 && WIFEXITED(status) && WEXITSTATUS(status)==0);
    /* Fatal exit cancels a sleeping timer before its process slot is reused. */
    for(i=0;i<8;++i){
        C(pipe(p)==0);pid=fork();C(pid>=0);
        if(!pid){close(p[0]);C(write(p[1],"s",1)==1);usleep(2000000UL);_exit(9);}
        close(p[1]);C(read(p[0],&byte,1)==1);close(p[0]);
        C(usleep(10000UL)==0);C(kill(pid,SIGKILL)==0);
        C(waitpid(pid,&status,0)==pid && WIFSIGNALED(status));
    }
    C(usleep(30000UL)==0);
    puts("finecheck: overlapping waiters, EINTR, alarms, spurious wakes and killed slot reuse passed");
}
static void computing(void) {
    int p[2],pid,status;
    char byte;
    unsigned long start,elapsed;
    C(pipe(p)==0);pid=fork();C(pid>=0);
    if(!pid){close(p[0]);C(write(p[1],"r",1)==1);for(;;)++spin;}
    close(p[1]);C(read(p[0],&byte,1)==1);close(p[0]);
    start=millis();C(usleep(20000UL)==0);elapsed=millis()-start;
    C(elapsed>=20 && elapsed<500);
    C(kill(pid,SIGKILL)==0 && waitpid(pid,&status,0)==pid && WIFSIGNALED(status));
    printf("FINE_BUSY_20 %lu ms (another process computing)\n",elapsed);
}
static void pause_recovery(void) {
    unsigned long start,elapsed;
    int pid=fork();C(pid>=0);
    if(!pid){C(usleep(30000UL)==0);puts("FINE_PAUSE_READY");fflush(stdout);_exit(0);}
    start=millis();C(usleep(400000UL)==0);elapsed=millis()-start;
    C(write(1,"FINE_RESUMED\n",13)==13);waited(pid);
    printf("FINE_PAUSED %lu ms\n",elapsed);puts("FINE_PAUSE_OK");
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"pause")) {pause_recovery();return 0;}
    if(argc>1 && !strcmp(argv[1],"idle")) {
        puts("FINE_IDLE_READY");fflush(stdout);C(usleep(2000000UL)==0);puts("FINE_IDLE_OK");return 0;
    }
    queue();clocks();timing();lifecycle();computing();printf("FINE_OK %u checks\n",checks);return 0;
}
