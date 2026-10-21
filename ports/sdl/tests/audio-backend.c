/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the production cooperative audio state machine with deterministic
 * clock/PCM services. Device writes copy samples synchronously, as specified. */
#include "SDL.h"
#include "audio.h"
#include "audio_formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"line %d: %s (%s)\n",__LINE__,#c,SDL_GetError()); exit(1); } } while(0)
static char error_text[256];
void SDL_SetError(const char *format, ...) {
    va_list args; va_start(args,format); vsnprintf(error_text,sizeof(error_text),format,args); va_end(args);
}
void SDL_Error(SDL_errorcode code) { SDL_SetError("SDL error %d", (int)code); }
char *SDL_GetError(void) { return error_text; }
void SDL_ClearError(void) { error_text[0]=0; }
size_t SDL_strlcpy(char *dst,const char *src,size_t n) {
    size_t length=strlen(src),copy=length;
    if(n){if(copy>=n)copy=n-1;memcpy(dst,src,copy);dst[copy]=0;}return length;
}
/* SDL_OpenAudio normally reaches this through SDL.c's subsystem dispatch. */
int SDL_InitSubSystem(Uint32 flags) { return flags == SDL_INIT_AUDIO ? SDL_AudioInit(NULL) : -1; }

static Uint32 now;
Uint32 SDL_GX_GetTicks(void) { return now; }
struct fake_pcm {
    int live, running, opens, closes, starts, pauses, writes, states;
    int fail_open, fail_state, fail_write, fail_start, fail_pause, bad_state, over_accept;
    Uint32 rate, channels, capacity, queued, zero_write, max_write, captured;
    Sint16 output[16384];
} device;
int SDL_PCMOpen(Sint32 rate,Uint8 channels,Uint32 capacity) {
    device.opens++;
    if(device.fail_open){SDL_SetError("scripted open failure");return -1;}
    CHECK(!device.live);device.live=1;device.running=0;device.rate=rate;
    device.channels=channels;device.capacity=capacity;device.queued=0;return 0;
}
void SDL_PCMClose(void) { device.closes++;device.live=device.running=0;device.queued=0; }
int SDL_PCMState(Uint32 *queued,Uint32 *capacity) {
    device.states++;
    if(device.fail_state){SDL_SetError("scripted state failure");return -1;}
    CHECK(device.live);*capacity=device.capacity;*queued=device.bad_state?device.capacity+1:device.queued;return 0;
}
Sint32 SDL_PCMWrite(const Sint16 *samples,Uint32 frames) {
    Uint32 take=frames,i;
    CHECK(device.live);device.writes++;
    if(device.fail_write){SDL_SetError("scripted write failure");return -1;}
    if(device.zero_write==(Uint32)device.writes)return 0;
    if(device.over_accept)return frames+1;
    if(device.max_write&&take>device.max_write)take=device.max_write;
    CHECK(take<=device.capacity-device.queued);
    CHECK(device.captured+take*device.channels<=16384UL);
    for(i=0;i<take*device.channels;i++)device.output[device.captured++]=samples[i];
    device.queued+=take;return take;
}
int SDL_PCMStart(void) {
    device.starts++;
    if(device.fail_start){SDL_SetError("scripted start failure");return -1;}
    CHECK(device.live);device.running=1;return 0;
}
int SDL_PCMPause(void) {
    device.pauses++;
    if(device.fail_pause){SDL_SetError("scripted pause failure");return -1;}
    CHECK(device.live);device.running=0;return 0;
}

enum { NORMAL, REENTER, CLOSE_IN_CALLBACK, PAUSE_IN_CALLBACK, LOCK_IN_CALLBACK, SILENCE };
static int callback_mode, callback_calls, callback_depth, max_depth;
static Uint32 produced;
static SDL_AudioSpec desired,obtained;
static Sint32 expected(Uint32 at,Uint16 format) {
    return (format&255U)==8 ? ((Sint32)(at%256)-128)*256 : (Sint32)at-500;
}
static void callback(void *userdata,Uint8 *data,int length) {
    Uint32 i,count;unsigned bytes=(desired.format&255U)/8U;
    CHECK(userdata==&device);CHECK(length==(int)desired.size);
    callback_calls++;callback_depth++;if(callback_depth>max_depth)max_depth=callback_depth;
    if(callback_mode==REENTER){
        SDL_AudioSpec before=desired;
        int calls=callback_calls,writes=device.writes;
        SDL_C16AudioPump();CHECK(callback_calls==calls&&device.writes==writes);
        CHECK(SDL_C16AudioWait(100)==100);
        CHECK(SDL_OpenAudio(&desired,NULL)==-1);CHECK(!memcmp(&desired,&before,sizeof(desired)));
        CHECK(SDL_AudioInit(NULL)==-1);
        SDL_PauseAudio(0);SDL_LockAudio();SDL_C16AudioPump();SDL_UnlockAudio();
        CHECK(callback_calls==calls&&device.writes==writes);
    }
    if(callback_mode==CLOSE_IN_CALLBACK){
        int closes=device.closes;
        SDL_CloseAudio();CHECK(device.closes==closes&&device.live);
        CHECK(SDL_GetAudioStatus()==SDL_AUDIO_STOPPED);CHECK(!SDL_C16AudioMixFormat());
        SDL_C16AudioPump();
        /* A deferred close must keep this borrowed storage alive until return. */
        memset(data,0x19,length*sizeof(*data));callback_depth--;return;
    }
    if(callback_mode==PAUSE_IN_CALLBACK)SDL_PauseAudio(1);
    if(callback_mode==LOCK_IN_CALLBACK)SDL_LockAudio();
    if(callback_mode!=SILENCE){
        count=(Uint32)length/bytes;
        for(i=0;i<count;i++){
            Sint32 value=(bytes==1)?(Sint32)(produced%256)-128:(Sint32)produced-500;
            if(!(desired.format&0x8000U))value+=bytes==1?128:32768L;
            SDL_C16AudioWriteSample(data+i*bytes,desired.format,value);produced++;
        }
    }
    callback_depth--;
}
static void reset(void) {
    SDL_AudioQuit();memset(&device,0,sizeof(device));memset(&desired,0,sizeof(desired));
    memset(&obtained,0,sizeof(obtained));device.max_write=7;now=1000;
    callback_mode=NORMAL;callback_calls=callback_depth=max_depth=0;produced=0;SDL_ClearError();
    desired.freq=44100;desired.format=AUDIO_S16MSB;desired.channels=2;desired.samples=8;
    desired.callback=callback;desired.userdata=&device;
}
static void open_audio(void) {
    int opens=device.opens,calls=callback_calls,starts=device.starts;
    CHECK(SDL_OpenAudio(&desired,&obtained)==0);CHECK(device.live&&device.opens==opens+1);
    CHECK(device.rate==(Uint32)desired.freq&&device.channels==desired.channels);
    CHECK(SDL_GetAudioStatus()==SDL_AUDIO_PAUSED);CHECK(callback_calls==calls&&device.starts==starts);
    CHECK(desired.size==(Uint32)desired.samples*desired.channels*((desired.format&255U)/8));
    CHECK(!memcmp(&desired,&obtained,sizeof(desired)));
}
static void verify_output(void) {
    Uint32 i;for(i=0;i<device.captured;i++)CHECK(device.output[i]==expected(i,desired.format));
}
int main(void) {
    int calls,writes,i;Uint32 captured;char name[32];
    Uint16 formats[]={AUDIO_U8,AUDIO_S8,AUDIO_U16LSB,AUDIO_S16LSB,AUDIO_U16MSB,AUDIO_S16MSB};
    reset();open_audio();device.zero_write=2;
    SDL_PauseAudio(0);CHECK(callback_calls==1&&produced==16&&device.captured==14);
    CHECK(device.writes==2&&device.starts==1&&SDL_GetAudioStatus()==SDL_AUDIO_PLAYING);verify_output();
    SDL_C16AudioPump();CHECK(callback_calls==3&&device.captured==48&&device.queued==24);verify_output();
    SDL_C16AudioPump();CHECK(callback_calls==3&&device.captured==48);
    CHECK(SDL_AudioDriverName(name,sizeof(name))==name&&!strcmp(name,"reference"));
    /* Invalid reopen cannot change the live device or caller's specification. */
    {SDL_AudioSpec bad=desired,before;bad.freq=1;before=bad;
     CHECK(SDL_OpenAudio(&bad,NULL)==-1&&!memcmp(&bad,&before,sizeof(bad)));
     CHECK(device.opens==1&&device.closes==0&&SDL_GetAudioStatus()==SDL_AUDIO_PLAYING);}
    CHECK(SDL_C16AudioWait(1000)==1);now+=0;CHECK(SDL_C16AudioWait(0)==0);
    calls=callback_calls;writes=device.writes;captured=device.captured;
    SDL_PauseAudio(1);device.queued=0;now+=100;
    SDL_C16AudioPump();CHECK(callback_calls==calls&&device.writes==writes&&device.captured==captured);
    CHECK(SDL_GetAudioStatus()==SDL_AUDIO_PAUSED&&SDL_C16AudioWait(99)==99);
    SDL_PauseAudio(0);CHECK(callback_calls==calls+3);verify_output();
    SDL_LockAudio();SDL_LockAudio();calls=callback_calls;device.queued=0;SDL_C16AudioPump();
    CHECK(callback_calls==calls&&SDL_C16AudioWait(99)==99);SDL_UnlockAudio();CHECK(callback_calls==calls);
    SDL_UnlockAudio();CHECK(callback_calls==calls+3);verify_output();
    SDL_CloseAudio();CHECK(!device.live&&device.closes==1&&SDL_GetAudioStatus()==SDL_AUDIO_STOPPED);
    SDL_CloseAudio();CHECK(device.closes==1);

    reset();desired.freq=48000;open_audio();CHECK(device.rate==48000UL&&desired.freq==48000L);
    callback_mode=REENTER;SDL_PauseAudio(0);CHECK(max_depth==1&&callback_calls==3);verify_output();SDL_CloseAudio();
    reset();open_audio();callback_mode=CLOSE_IN_CALLBACK;SDL_PauseAudio(0);
    CHECK(callback_calls==1&&!device.live&&device.closes==1&&!device.starts&&!device.captured);
    CHECK(SDL_GetAudioStatus()==SDL_AUDIO_STOPPED);callback_mode=NORMAL;open_audio();SDL_CloseAudio();

    reset();open_audio();callback_mode=PAUSE_IN_CALLBACK;SDL_PauseAudio(0);
    CHECK(callback_calls==1&&!device.captured&&!device.starts&&SDL_GetAudioStatus()==SDL_AUDIO_PAUSED);
    callback_mode=NORMAL;produced=0;SDL_PauseAudio(0);CHECK(callback_calls==4);verify_output();SDL_CloseAudio();
    reset();open_audio();callback_mode=LOCK_IN_CALLBACK;SDL_PauseAudio(0);
    CHECK(callback_calls==1&&!device.captured);callback_mode=NORMAL;SDL_UnlockAudio();
    CHECK(callback_calls==3&&device.captured==48);verify_output();SDL_CloseAudio();

    /* Driver failures at every service transition release the exclusive lease. */
    for(i=0;i<6;i++){
        reset();open_audio();
        switch(i){case 0:device.fail_state=1;break;case 1:device.fail_write=1;break;
        case 2:device.fail_start=1;break;case 3:device.bad_state=1;break;case 4:device.over_accept=1;break;}
        SDL_PauseAudio(0);if(i==5){device.fail_pause=1;SDL_PauseAudio(1);}
        CHECK(SDL_GetAudioStatus()==SDL_AUDIO_STOPPED&&!device.live&&device.closes==1);
        CHECK(*SDL_GetError());SDL_C16AudioPump();CHECK(device.closes==1);SDL_CloseAudio();CHECK(device.closes==1);
    }
    reset();device.fail_open=1;{SDL_AudioSpec before=desired;CHECK(SDL_OpenAudio(&desired,NULL)==-1);
        CHECK(!memcmp(&desired,&before,sizeof(desired))&&!device.live&&SDL_GetAudioStatus()==SDL_AUDIO_STOPPED);}
    reset();desired.freq=123;{SDL_AudioSpec before=desired;CHECK(SDL_OpenAudio(&desired,NULL)==-1);
        CHECK(!memcmp(&desired,&before,sizeof(desired))&&!device.live&&!device.opens);}

    for(i=0;i<6;i++){
        reset();desired.format=formats[i];desired.channels=1;open_audio();SDL_PauseAudio(0);
        CHECK(device.captured==24);verify_output();SDL_CloseAudio();
        reset();desired.format=formats[i];callback_mode=SILENCE;open_audio();SDL_PauseAudio(0);
        for(captured=0;captured<device.captured;captured++)CHECK(device.output[captured]==0);
        SDL_CloseAudio();
    }
    reset();open_audio();SDL_PauseAudio(0);device.queued=0;now+=3000;
    CHECK(SDL_C16AudioWait(100)==1);SDL_CloseAudio();SDL_AudioQuit();CHECK(SDL_AudioDriverName(name,sizeof(name))==NULL);
    printf("PASS: %u production audio lifecycle, partial-write, callback, lease, rate and sample checks\n",checks);
    return 0;
}
