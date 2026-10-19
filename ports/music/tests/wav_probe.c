#include "../wav.h"
#include <stdio.h>
int main(int argc,char **argv) {
    Wav w;short samples[226];long n;unsigned i;unsigned long hash=2166136261UL;
    char error[160];
    if(argc!=2)return 2;
    if(!wav_open(&w,argv[1],error,sizeof(error))){puts(error);return 1;}
    while((n=wav_read(&w,samples,113))>0)for(i=0;i<(unsigned)n*w.channels;i++)hash=((hash^(unsigned short)samples[i])*16777619UL)&0xffffffffUL;
    if(n<0)return 3;
    if(!wav_seek(&w,0)||wav_read(&w,samples,1)!=1)return 4;
    printf("%lu %u %u %lu %lu %d\n",w.rate,w.channels,w.bits,w.frames,hash,(int)samples[0]);
    wav_close(&w);return 0;
}
