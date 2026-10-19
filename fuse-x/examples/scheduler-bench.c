/* CPU contention: loops must schedule, and signals must reach pure computation. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <stdlib.h>
static volatile unsigned long counter;
static volatile int done;
static void stop(int sig) { (void)sig; done = 1; }
static unsigned long mix(unsigned long n) { return n * 1664525UL + 1013904223UL; }
static unsigned long millis(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))exit(2);
    return (unsigned long)t.tv_sec * 1000UL + t.tv_nsec/1000000L;
}
static void check(int ok) { if(!ok){perror("scheduler probe");exit(1);} }
int main(void) {
    int children[2], gate[2], status, i;
    unsigned long start, elapsed;
    char byte;
    check(pipe(gate)==0);
    for(i=0;i<2;++i) {
        children[i]=fork();check(children[i]>=0);
        if(!children[i]) {
            signal(SIGUSR1,stop);close(gate[0]);
            check(write(gate[1],"r",1)==1);
            while(!done) {counter=mix(counter);}
            _exit(counter ? 0 : 7);
        }
    }
    close(gate[1]);for(i=0;i<2;++i)check(read(gate[0],&byte,1)==1);close(gate[0]);
    start=millis();check(usleep(60000UL)==0);elapsed=millis()-start;
    check(elapsed>=60 && elapsed<1500);
    printf("MEASURE sleep_with_two_cpu_tasks 1 %lu\n",elapsed*1000UL);
    start=millis();for(i=0;i<1000;++i)check(getpid()>0);elapsed=millis()-start;
    printf("MEASURE getpid_with_two_cpu_tasks 1000 %lu\n",elapsed*1000UL);
    for(i=0;i<2;++i)check(kill(children[i],SIGUSR1)==0);
    for(i=0;i<2;++i)check(waitpid(children[i],&status,0)==children[i] && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("PROBE_OK");return 0;
}
