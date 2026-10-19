/* Streaming RIFF/WAVE PCM reader. File octets occupy individual C16 cells. */
#include "wav.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
static unsigned word(const unsigned char *p){return (unsigned)p[0]|((unsigned)p[1]<<8);}
static unsigned long wide(const unsigned char *p){return (unsigned long)word(p)|((unsigned long)word(p+2)<<16);}
static int octets(int fd,unsigned char *p,unsigned count) {
    long n;
    while(count){n=read(fd,p,count);if(n<0&&errno==EINTR)continue;if(n<=0)return 0;
        p+=n;count-=(unsigned)n;}
    return 1;
}
static int exact(int fd,unsigned char *p,unsigned count) {
    unsigned i;if(!octets(fd,p,count))return 0;
    for(i=0;i<count;i++)if((unsigned)p[i]>255U)return 0;
    return 1;
}
static int failure(Wav *w,char *error,unsigned limit,const char *message) {
    if(limit)snprintf(error,limit,"%s",message);wav_close(w);return 0;
}
void wav_close(Wav *w){if(w->fd>=0)close(w->fd);w->fd=-1;}
int wav_open(Wav *w,const char *path,char *error,unsigned limit) {
    unsigned char head[16];struct stat st;unsigned fmt=0,data=0,tag=0;
    unsigned long end,at=12UL,size,body,next,byte_rate=0;
    memset(w,0,sizeof(*w));w->fd=-1;
    w->fd=open(path,O_RDONLY|O_CLOEXEC);
    if(w->fd<0)return failure(w,error,limit,"Cannot open music file");
    if(fstat(w->fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<12||!exact(w->fd,head,12)||
       memcmp(head,"RIFF",4)||memcmp(head+8,"WAVE",4))return failure(w,error,limit,"Expected a RIFF/WAVE file");
    size=wide(head+4);
    if(size<4UL||size>0x7ffffff7UL||(end=size+8UL)>(unsigned long)st.st_size)
        return failure(w,error,limit,"Truncated or oversized RIFF file");
    while(at<end) {
        if(end-at<8UL||lseek(w->fd,(long)at,SEEK_SET)<0||!exact(w->fd,head,8))
            return failure(w,error,limit,"Truncated WAV chunk header");
        size=wide(head+4);body=at+8UL;
        if(size>end-body)return failure(w,error,limit,"WAV chunk exceeds RIFF bounds");
        next=body+size+(size&1UL);
        if(next>end)return failure(w,error,limit,"Missing WAV chunk padding");
        if(!memcmp(head,"fmt ",4)) {
            if(fmt||size<16UL||!exact(w->fd,head,16))return failure(w,error,limit,"Invalid WAV format chunk");
            fmt=1;tag=word(head);w->channels=word(head+2);w->rate=wide(head+4);
            byte_rate=wide(head+8);w->align=word(head+12);w->bits=word(head+14);
        } else if(!memcmp(head,"data",4)) {
            if(data)return failure(w,error,limit,"Multiple WAV data chunks are unsupported");
            data=1;w->offset=body;w->bytes=size;
        }
        at=next;
    }
    if(!fmt||!data||tag!=1||w->channels<1||w->channels>2||(w->bits!=8&&w->bits!=16&&w->bits!=24))
        return failure(w,error,limit,"Use PCM 8/16/24-bit mono or stereo WAV");
    if(w->rate<8000UL||w->rate>96000UL||w->align!=w->channels*(w->bits/8U)||
       byte_rate!=w->rate*w->align||!w->bytes||w->bytes%w->align)
        return failure(w,error,limit,"Invalid WAV rate, alignment or data length");
    w->frames=w->bytes/w->align;
    if(!wav_seek(w,0))return failure(w,error,limit,"Cannot seek WAV data");
    return 1;
}
int wav_seek(Wav *w,unsigned long frame) {
    if(frame>w->frames||lseek(w->fd,(long)(w->offset+frame*w->align),SEEK_SET)<0)return 0;
    w->position=frame;return 1;
}
long wav_read(Wav *w,short *out,unsigned frames) {
    unsigned char input[6144];unsigned n,i,lo,mid,hi;
    if(frames>1024U)frames=1024U;
    if((unsigned long)frames>w->frames-w->position)frames=(unsigned)(w->frames-w->position);
    if(!frames)return 0;
    n=frames*w->align;
    if(!octets(w->fd,input,n))return -1;
    n=frames*w->channels;
    /* Conversion and octet validation share one pass. Both targets use
     * two's-complement 16-bit short, so narrowing preserves the PCM bit pattern. */
    if(w->bits==16)for(i=0;i<n;i++){
        lo=input[2U*i];hi=input[2U*i+1U];if((lo|hi)>255U)return -1;
        out[i]=(short)(lo|(hi<<8));
    }
    else if(w->bits==24)for(i=0;i<n;i++){
        lo=input[3U*i];mid=input[3U*i+1U];hi=input[3U*i+2U];
        if((lo|mid|hi)>255U)return -1;
        /* Keep the upper 16 bits, equivalent to a signed shift by 8.
         * No rounding or dither; validate the discarded octet as well. */
        out[i]=(short)(mid|(hi<<8));
    }
    else for(i=0;i<n;i++){
        lo=input[i];if(lo>255U)return -1;out[i]=(short)((lo^128U)<<8);
    }
    w->position+=frames;return frames;
}
