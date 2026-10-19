#include "music.h"
#include "wav.h"
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <sys/pcm.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static unsigned long now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
int main(int argc,char **argv) {
    void *player;unsigned state;struct timeval wait;Wav wav;char error[160];
    if(argc==3&&(!strcmp(argv[1],"--benchmark")||!strcmp(argv[1],"--benchmark-output"))) {
        short samples[2048];long frames,written;unsigned long start,elapsed,hash=0,speed;unsigned at,count;
        int pcm=-1;struct pcm_format format;
        if(!wav_open(&wav,argv[2],error,sizeof(error))){fprintf(stderr,"%s\n",error);return 1;}
        if(!strcmp(argv[1],"--benchmark-output")) {
            pcm=open("/dev/pcm",O_WRONLY|O_NONBLOCK);
            format.version=1;format.channels=wav.channels;format.rate=wav.rate;format.capacity=wav.rate*2UL;
            if(pcm<0||ioctl(pcm,PCM_CONFIGURE,&format)<0){wav_close(&wav);if(pcm>=0)close(pcm);return 1;}
        }
        start=now();
        while((frames=wav_read(&wav,samples,1024))>0){
            hash+=(unsigned short)samples[0]^(unsigned short)samples[((unsigned)frames-1)*wav.channels];
            if(pcm>=0){
                at=0;count=(unsigned)frames*wav.channels;
                while(at<count){
                    written=write(pcm,samples+at,count-at);
                    if(written<0&&errno==EAGAIN){if(ioctl(pcm,PCM_STOP,0)<0){frames=-1;break;}continue;}
                    if(written<=0){frames=-1;break;}at+=(unsigned)written;
                }
                if(frames<0)break;
            }
        }
        elapsed=now()-start;if(!elapsed)elapsed=1;
        speed=(unsigned long)((unsigned long long)wav.frames*100000ULL/elapsed/wav.rate);
        if(pcm>=0)close(pcm);
        if(frames<0){wav_close(&wav);return 1;}
        printf("WAV %sbenchmark: %lu frames in %lu ms; %lu.%02lux real time; checksum %lu\n",pcm>=0?"pipeline ":"",wav.frames,elapsed,speed/100UL,speed%100UL,hash);
        wav_close(&wav);return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--info")) {
        if(!wav_open(&wav,argv[2],error,sizeof(error))){fprintf(stderr,"%s\n",error);return 1;}
        printf("PCM WAV: %lu Hz, %u channels, %u bits, %lu frames\n",wav.rate,wav.channels,wav.bits,wav.frames);
        wav_close(&wav);return 0;
    }
    if(argc!=2){fprintf(stderr,"usage: wavplay [--info|--benchmark|--benchmark-output] FILE.wav\n");return 1;}
    player=music_new();if(!player)return 1;
    music_volume(player,100);
    if(!music_load(player,argv[1])){fprintf(stderr,"%s\n",music_status(player));music_free(player);return 1;}
    do {
        music_step(player);state=music_state(player);
        if(state!=1)break;
        wait.tv_sec=0;wait.tv_usec=10000;select(0,0,0,0,&wait);
    }while(1);
    if(state==3)printf("WAV complete: %lu ms, %lu missing frames\n",music_duration(player),music_underruns(player));
    else fprintf(stderr,"%s\n",music_status(player));
    music_free(player);return state==3?0:1;
}
