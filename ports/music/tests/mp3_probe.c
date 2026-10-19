/* Exploratory minimp3 decode/throughput probe; no host codec or audio device. */
#include "minimp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
static mp3dec_t decoder;
static unsigned char input[16384];
static mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static unsigned char octets[MINIMP3_MAX_SAMPLES_PER_FRAME*2];
static unsigned long micros(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (unsigned long)t.tv_sec*1000000UL+t.tv_nsec/1000UL;
}
int main(int argc,char **argv) {
    int fd,out=-1,eof=0;long got;int32_t samples;
    unsigned used=0,i,blocks=0,limit=argc>2?(unsigned)atoi(argv[2]):12;
    unsigned long start,elapsed,excluded=0,stamp,frames=0,hash=2166136261UL,rate=0,channels=0;
    mp3dec_frame_info_t info;
    if(argc<2||!limit||limit>500)return 1;
    fd=open(argv[1],O_RDONLY);if(fd<0)return 1;
    if(argc>3){out=open(argv[3],O_WRONLY|O_CREAT|O_TRUNC,0600);if(out<0){close(fd);return 1;}}
    mp3dec_init(&decoder);start=micros();
    while(blocks<limit) {
        if(!eof&&used<sizeof(input)) {
            got=read(fd,input+used,sizeof(input)-used);
            if(got<0){close(fd);return 2;}
            if(!got)eof=1;
            else {
                for(i=used;i<used+(unsigned)got;i++)if(input[i]>255U){close(fd);return 3;}
                used+=(unsigned)got;
            }
        }
        if(!used)break;
        samples=mp3dec_decode_frame(&decoder,input,used,pcm,&info);
        if(info.frame_bytes<0||info.frame_bytes>used||samples<0||samples>1152||
           (samples&&(info.channels<1||info.channels>2||info.hz<=0))){close(fd);return 4;}
        if(!info.frame_bytes)break;
        used-=(unsigned)info.frame_bytes;memmove(input,input+info.frame_bytes,used);
        if(!samples)continue;
        if(rate&&(rate!=(unsigned long)info.hz||channels!=(unsigned long)info.channels)){close(fd);return 5;}
        rate=info.hz;channels=info.channels;frames+=(unsigned)samples;blocks++;
        for(i=0;i<(unsigned)samples*(unsigned)info.channels;i++){
            hash=((hash^(unsigned short)pcm[i])*16777619UL)&0xffffffffUL;
            octets[2U*i]=(unsigned short)pcm[i]&255U;
            octets[2U*i+1U]=(unsigned short)pcm[i]>>8;
        }
        if(out>=0){
            stamp=micros();
            if(write(out,octets,(unsigned)samples*(unsigned)info.channels*2U)!=(long)((unsigned)samples*(unsigned)info.channels*2U)){
                close(out);close(fd);return 7;
            }
            excluded+=micros()-stamp;
        }
    }
    elapsed=micros()-start-excluded;close(fd);if(out>=0)close(out);
    if(!frames)return 6;
    printf("MP3_PROBE %lu %lu %lu %lu %lu\n",rate,channels,frames,elapsed,hash);
    return 0;
}
