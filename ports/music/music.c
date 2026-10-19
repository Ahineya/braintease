/* The UI only schedules bounded progress; all sample handling stays in C. */
#include "music.h"
#include "wav.h"
#include "../../ports/fuzix/ripple/include/sys/pcm.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef struct Player {
    Wav wav;int pcm;
    unsigned state,volume,started,ending,used,offset;
    unsigned long origin,position,underruns;
    short samples[2048];char status[160],title[256];
} Player;
static void error(Player *p,const char *message){snprintf(p->status,sizeof(p->status),"%s",message);p->state=4;}
void *music_new(void) {
    Player *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->wav.fd=p->pcm=-1;p->volume=75;strcpy(p->status,"Choose a WAV file and press Enter");
    return p;
}
void music_free(void *pointer) {
    Player *p=pointer;if(!p)return;
    wav_close(&p->wav);if(p->pcm>=0)close(p->pcm);free(p);
}
void music_stop(void *pointer) {
    Player *p=pointer;
    if(p->pcm>=0){ioctl(p->pcm,PCM_STOP,0);close(p->pcm);p->pcm=-1;}
    wav_close(&p->wav);memset(&p->wav,0,sizeof(p->wav));p->wav.fd=-1;
    p->state=p->started=p->ending=p->used=p->offset=0;
    p->origin=p->position=p->underruns=0;p->title[0]=0;
    strcpy(p->status,"Stopped");
}
static int configure(Player *p) {
    struct pcm_format f;unsigned gain;
    if(p->pcm<0)p->pcm=open("/dev/pcm",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if(p->pcm<0){error(p,errno==EBUSY?"Audio device is busy":"Cannot open /dev/pcm");return 0;}
    if(ioctl(p->pcm,PCM_STOP,0)<0){error(p,"Cannot stop PCM device");return 0;}
    f.version=PCM_VERSION;f.channels=p->wav.channels;f.rate=p->wav.rate;f.capacity=f.rate;
    if(ioctl(p->pcm,PCM_CONFIGURE,&f)<0){error(p,"Cannot configure PCM format");return 0;}
    gain=(unsigned)((unsigned long)p->volume*65535UL/100UL);
    if(ioctl(p->pcm,PCM_GAIN,&gain)<0){error(p,"Cannot set PCM volume");return 0;}
    p->started=p->ending=p->used=p->offset=0;p->underruns=0;
    return 1;
}
int music_load(void *pointer,const char *path) {
    Player *p=pointer;Wav next;char message[160];unsigned i;
    if(strlen(path)>255U){snprintf(p->status,sizeof(p->status),"Music path is too long");return 0;}
    if(!wav_open(&next,path,message,sizeof(message))){snprintf(p->status,sizeof(p->status),"%s",message);return 0;}
    wav_close(&p->wav);p->wav=next;p->origin=p->position=0;
    if(!configure(p))return 0;
    strncpy(p->title,path,sizeof(p->title)-1);
    for(i=0;p->title[i];i++)if((unsigned char)p->title[i]<32U||p->title[i]==127)p->title[i]='?';
    p->state=1;strcpy(p->status,"Buffering WAV audio");music_step(p);return 1;
}
void music_step(void *pointer) {
    Player *p=pointer;struct pcm_state s;long n;unsigned budget=16;
    if(p->state!=1)return;
    if(ioctl(p->pcm,PCM_STATE,&s)<0||s.flags&PCM_ERROR){error(p,"PCM output failed");return;}
    p->position=p->origin+s.position;if(p->position>p->wav.frames)p->position=p->wav.frames;
    p->underruns=s.underruns;
    if(s.flags&PCM_DONE){p->position=p->wav.frames;p->state=3;strcpy(p->status,"Finished");return;}
    while(budget--&&!p->ending) {
        if(p->offset<p->used) {
            n=write(p->pcm,p->samples+p->offset,p->used-p->offset);
            if(n<0&&(errno==EAGAIN||errno==EINTR))break;
            if(n<=0){error(p,"PCM sample submission failed");return;}
            p->offset+=(unsigned)n;
        } else {
            n=wav_read(&p->wav,p->samples,1024);
            if(n<0){error(p,"WAV data was truncated or contains invalid octets");return;}
            p->offset=0;p->used=(unsigned)n*p->wav.channels;
            if(!n)break;
        }
    }
    if(ioctl(p->pcm,PCM_STATE,&s)<0){error(p,"Cannot read PCM state");return;}
    if(!p->started&&(s.queued>=s.capacity*3UL/4UL||(p->wav.position==p->wav.frames&&p->offset==p->used))) {
        if(p->wav.position==p->wav.frames&&p->offset==p->used){
            if(ioctl(p->pcm,PCM_END,0)<0){error(p,"Cannot drain PCM output");return;}p->ending=1;
        }
        if(ioctl(p->pcm,PCM_START,0)<0){error(p,"Cannot start PCM playback");return;}
        p->started=1;strcpy(p->status,"Playing WAV");
    }
    if(p->started&&!p->ending&&p->wav.position==p->wav.frames&&p->offset==p->used) {
        if(ioctl(p->pcm,PCM_END,0)<0){error(p,"Cannot drain PCM output");return;}
        p->ending=1;
    }
}
void music_pause(void *pointer) {
    Player *p=pointer;
    if(p->state==1){if(p->started&&ioctl(p->pcm,PCM_PAUSE,0)<0){error(p,"Cannot pause audio");return;}p->state=2;strcpy(p->status,"Paused");}
    else if(p->state==2){if(p->started&&ioctl(p->pcm,PCM_START,0)<0){error(p,"Cannot resume audio");return;}p->state=1;strcpy(p->status,"Playing WAV");}
}
int music_seek(void *pointer,long seconds) {
    Player *p=pointer;unsigned old=p->state;unsigned long target,amount;
    if(!p->wav.frames||p->state==4)return 0;
    if(seconds>3600L)seconds=3600L;if(seconds< -3600L)seconds= -3600L;
    amount=(unsigned long)(seconds<0?-seconds:seconds)*p->wav.rate;
    target=seconds<0?(amount>p->position?0:p->position-amount):
        (amount>p->wav.frames-p->position?p->wav.frames:p->position+amount);
    if(!wav_seek(&p->wav,target)){error(p,"Cannot seek music file");return 0;}
    if(!configure(p))return 0;p->origin=p->position=target;p->state=old==2?2:1;
    strcpy(p->status,"Seek complete");return 1;
}
void music_volume(void *pointer,unsigned percent) {
    Player *p=pointer;unsigned gain;if(percent>100)percent=100;p->volume=percent;
    if(p->pcm>=0){gain=(unsigned)((unsigned long)percent*65535UL/100UL);if(ioctl(p->pcm,PCM_GAIN,&gain)<0)error(p,"Cannot change volume");}
}
unsigned music_state(void *p){return ((Player *)p)->state;}
unsigned music_gain(void *p){return ((Player *)p)->volume;}
unsigned music_channels(void *p){return ((Player *)p)->wav.channels;}
unsigned long music_rate(void *p){return ((Player *)p)->wav.rate;}
static unsigned long milliseconds(unsigned long frame,unsigned long rate){return rate?(frame/rate)*1000UL+(frame%rate)*1000UL/rate:0;}
unsigned long music_elapsed(void *p){Player *v=p;return milliseconds(v->position,v->wav.rate);}
unsigned long music_duration(void *p){Player *v=p;return milliseconds(v->wav.frames,v->wav.rate);}
unsigned long music_underruns(void *p){return ((Player *)p)->underruns;}
const char *music_status(void *p){return ((Player *)p)->status;}
const char *music_title(void *p){return ((Player *)p)->title;}
