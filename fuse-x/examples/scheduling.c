/* Hosted scheduling regression: wall-clock wakeups, CPU load and PCM sink gaps. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/pcm.h>
#include "wav.h"

static unsigned long now(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;
}
static void tick(void) {
    struct timeval wait;
    wait.tv_sec=0;wait.tv_usec=10000;
    select(0,0,0,0,&wait);
}
static int latency(void) {
    unsigned long start=now(),previous=start,current,gap,maximum=0,count=0;
    puts("LATENCY_READY");fflush(stdout);
    do {
        tick();current=now();gap=current-previous;previous=current;
        if(gap>maximum)maximum=gap;
        ++count;
    }while(current-start<20000UL);
    printf("LATENCY_DONE max_gap_ms=%lu samples=%lu\n",maximum,count);
    return 0;
}
static int audio(const char *path) {
    Wav wav;char error[160];short samples[1024];
    struct pcm_format format;struct pcm_state state;struct pcm_output_stats host;
    unsigned offset=0,used=0,started=0,ending=0;long count;
    int fd=open("/dev/pcm",O_WRONLY|O_NONBLOCK);
    if(fd<0||!wav_open(&wav,path,error,sizeof(error)))return 1;
    format.version=PCM_VERSION;format.channels=1;format.rate=wav.rate;format.capacity=wav.rate;
    if(wav.channels!=1||ioctl(fd,PCM_CONFIGURE,&format)<0)return 1;
    for(;;) {
        if(ioctl(fd,PCM_STATE,&state)<0||state.flags&PCM_ERROR)return 1;
        if(state.flags&PCM_DONE)break;
        if(!ending) {
            if(offset==used) {
                count=wav_read(&wav,samples,1024);
                if(count<0)return 1;
                used=(unsigned)count;offset=0;
                if(!used) {
                    if(ioctl(fd,PCM_END,0)<0)return 1;
                    ending=1;
                }
            }
            if(offset<used) {
                count=write(fd,samples+offset,used-offset);
                if(count<0&&errno!=EAGAIN&&errno!=EINTR)return 1;
                if(count>0)offset+=(unsigned)count;
            }
        }
        if(!started&&(state.queued>=format.capacity*3UL/4UL||ending)) {
            if(ioctl(fd,PCM_START,0)<0)return 1;
            started=1;puts("AUDIO_READY");fflush(stdout);
        }
        /* Fill quickly, then pace refills like the production music player. */
        if(started)tick();
    }
    if(ioctl(fd,PCM_OUTPUT_STATS,&host)<0)return 1;
    printf("AUDIO_DONE logical=%lu late_ms=%lu live=%u host_gaps=%lu host_max_gap_ms=%lu host_missing_frames=%lu\n",
        state.underruns,state.late_ms,host.live,host.gaps,host.max_gap_ms,(unsigned long)host.missing_frames);
    wav_close(&wav);close(fd);return 0;
}
int main(int argc,char **argv) {
    unsigned long start,value=1;unsigned i;
    if(argc==2&&!strcmp(argv[1],"latency"))return latency();
    if(argc==3&&!strcmp(argv[1],"audio"))return audio(argv[2]);
    if(argc==2&&!strcmp(argv[1],"cpu")) {
        start=now();
        do {for(i=0;i<60000;i++)value=value*1664525UL+1013904223UL;}while(now()-start<3000UL);
        printf("CPU_DONE %lu\n",value);return 0;
    }
    if(argc==2&&!strcmp(argv[1],"terminal")) {
        for(i=0;i<2000;i++)printf("\033[1;36mRedrawing mux row %u: background audio must keep running\033[0m\n",i);
        puts("TERMINAL_DONE");return 0;
    }
    return 2;
}
