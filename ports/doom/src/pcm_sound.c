/* PCM sound effects for Ripple and an independent host C capture oracle.
 * DMX records stay pinned while cached; no audio work is built into BF Doom. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doom_port.h"
#ifdef DOOM_GUEST
#include "audio.h"
#elif defined(RIPPLE) || defined(RCC_NATIVE)
#include <vm_audio.h>
#endif
#include "pcm_sound.h"
#define PCM_RATE 22050UL
#define PCM_FRAMES 630U /* Exactly 22050 / 35; one block for the next tick. */
#define PCM_VOICES 16

typedef struct PcmSound {
    struct PcmSound *next;
    byte *data;
    uint32_t length, step;
    int lump;
} PcmSound;
typedef struct {
    PcmSound *sound;
    uint32_t offset, fraction;
    int left, right;
} PcmVoice;
static PcmSound *cache;
static PcmVoice voices[PCM_VOICES];
static short mixed[PCM_FRAMES * 2];
static uint32_t mixed_tick;
static int initialized, prefixed;
#if !defined(RIPPLE) && !defined(RCC_NATIVE) && !defined(DOOM_GUEST)
static FILE *capture;
static short pending[PCM_FRAMES * 2];
#endif

static uint32_t read32(const byte *p) {
    return (uint32_t)(p[0]&255U) | ((uint32_t)(p[1]&255U)<<8)
        | ((uint32_t)(p[2]&255U)<<16) | ((uint32_t)(p[3]&255U)<<24);
}
static PcmSound *load_sound(int lump) {
    PcmSound *sound;
    uint32_t length, count, rate;
    byte *data;
    for (sound=cache;sound;sound=sound->next) if(sound->lump==lump)return sound;
    length=W_LumpLength((unsigned int)lump);
    if(length<8UL)return NULL;
    data=W_CacheLumpNum(lump,PU_STATIC);
    rate=(uint32_t)(data[2]&255U) | ((uint32_t)(data[3]&255U)<<8);
    count=read32(data+4);
    if((data[0]&255U)!=3U || (data[1]&255U)!=0U || rate<4000UL || rate>48000UL || count<=32UL || count>length-8UL) {
        W_ReleaseLumpNum(lump);return NULL;
    }
    sound=malloc(sizeof(*sound));
    if(!sound) {W_ReleaseLumpNum(lump);return NULL;}
    /* DMX includes 16 guard samples on either side of the actual waveform. */
    sound->data=data+24;sound->length=count-32UL;
    sound->step=(rate*65536UL)/PCM_RATE;sound->lump=lump;
    sound->next=cache;cache=sound;return sound;
}
static boolean pcm_init(boolean prefix) {
#ifdef DOOM_GUEST
    if (!Doom_GuestAudioOpen(PCM_RATE, PCM_FRAMES)) return false;
#elif defined(RIPPLE) || defined(RCC_NATIVE)
    VmDeviceCaps caps;
    if(vm_devices_caps(&caps)!=VM_DEVICE_OK || !(caps.bits&VM_CAP_PCM))return false;
    if(vm_audio_configure(PCM_RATE,2,PCM_FRAMES*4UL)!=VM_DEVICE_OK)return false;
#else
    const char *path=getenv("DOOM_PCM_RAW");
    capture=path?fopen(path,"wb"):NULL;
    if(path && !capture)I_Error("Cannot open native PCM capture");
    memset(pending,0,sizeof(pending));
#endif
    memset(voices,0,sizeof(voices));memset(mixed,0,sizeof(mixed));
    prefixed=prefix;mixed_tick=Doom_Tick();initialized=1;
#ifdef DOOM_GUEST
    Doom_GuestAudioSubmit(mixed, PCM_FRAMES);
    Doom_GuestAudioStart();
#elif defined(RIPPLE) || defined(RCC_NATIVE)
    if(vm_audio_submit(mixed,PCM_FRAMES)!=VM_DEVICE_OK || vm_audio_control(VM_AUDIO_START,0)!=VM_DEVICE_OK)I_Error("Cannot start PCM output");
#endif
    return true;
}
static void pcm_shutdown(void) {
    PcmSound *sound;
    if(!initialized)return;
#ifdef DOOM_GUEST
    Doom_GuestAudioClose();
#elif defined(RIPPLE) || defined(RCC_NATIVE)
    vm_audio_control(VM_AUDIO_STOP,0);
#else
    if(capture && fclose(capture))I_Error("Cannot finish native PCM capture");
    capture=NULL;
#endif
    memset(voices,0,sizeof(voices));
    while(cache) {sound=cache;cache=sound->next;W_ReleaseLumpNum(sound->lump);free(sound);}
    initialized=0;
}
/* Music can own the shared audio transport when -nosfx is requested. */
boolean Doom_PcmEnsure(void) { return initialized || pcm_init(true); }
static void pcm_name(sfxinfo_t *sfx,char *name) {
    unsigned int i,offset=prefixed?2:0,limit=prefixed?6:8;
    if(sfx->link)sfx=sfx->link;
    if(prefixed){name[0]='d';name[1]='s';}
    for(i=0;i<limit && sfx->name[i];++i)name[offset+i]=sfx->name[i];
    name[offset+i]=0;
}
static int pcm_lump(sfxinfo_t *sfx) {
    char name[9];pcm_name(sfx,name);
    return W_GetNumForName(name);
}
#ifdef DOOM_GUEST
static void pcm_cache(sfxinfo_t *sounds,int count) {
    int i,lump;char name[9];
    if(!initialized)return;
    /* File reads and zone allocation must not happen on the first gunshot.
     * Shareware IWADs omit some registered-game sounds; skip absent lumps. */
    for(i=1;i<count;++i) {
        pcm_name(&sounds[i],name);lump=W_CheckNumForName(name);
        if(lump>=0)load_sound(lump);
    }
}
#endif
static void pcm_params(int channel,int volume,int separation) {
    if(channel<0 || channel>=PCM_VOICES)return;
    if(volume<0)volume=0;
    if(volume>127)volume=127;
    if(separation<0)separation=0;
    if(separation>254)separation=254;
    voices[channel].left=volume*(254-separation)/254;
    voices[channel].right=volume*separation/254;
}
static int pcm_start(sfxinfo_t *sfx,int channel,int volume,int separation) {
    PcmSound *sound;
    if(!initialized || channel<0 || channel>=PCM_VOICES)return -1;
    sound=load_sound(sfx->lumpnum);
    voices[channel].sound=sound;voices[channel].offset=voices[channel].fraction=0;
    pcm_params(channel,volume,separation);
    return sound?channel:-1;
}
static void pcm_stop(int channel) {if(channel>=0 && channel<PCM_VOICES)voices[channel].sound=NULL;}
static boolean pcm_playing(int channel) {
    return channel>=0 && channel<PCM_VOICES && voices[channel].sound
        && voices[channel].offset<voices[channel].sound->length;
}
static short clip(int32_t value) {if(value>32767L)return 32767;if(value< -32768L)return (short)-32768;return (short)value;}
static void mix_block(void) {
    unsigned int frame,channel,count=0;
    PcmVoice *active[PCM_VOICES];
    for(channel=0;channel<PCM_VOICES;++channel)
        if(voices[channel].sound && voices[channel].offset<voices[channel].sound->length)
            active[count++]=&voices[channel];
    if(!count){memset(mixed,0,sizeof(mixed));return;}
#ifdef DOOM_GUEST
    {
        struct pcm_mix_voice input[PCM_VOICES];
        for (channel = 0; channel < count; ++channel) {
            PcmVoice *v = active[channel];
            input[channel].data = v->sound->data;
            input[channel].length = v->sound->length;
            input[channel].offset = v->offset; input[channel].fraction = v->fraction;
            input[channel].step = v->sound->step;
            input[channel].left = v->left; input[channel].right = v->right;
        }
        if (Doom_GuestAudioMix(input, count, mixed, PCM_FRAMES)) {
            for (channel = 0; channel < count; ++channel) {
                active[channel]->offset = input[channel].offset;
                active[channel]->fraction = input[channel].fraction;
            }
            return;
        }
    }
#endif
    for(frame=0;frame<PCM_FRAMES;++frame) {
        int32_t left=0,right=0;
        for(channel=0;channel<count;++channel) {
            PcmVoice *v=active[channel];
            int32_t value;
            uint32_t next;
            if(!v->sound || v->offset>=v->sound->length)continue;
            value=((int)(v->sound->data[v->offset]&255U)-128)*256L;
            left+=value*v->left/127L;right+=value*v->right/127L;
            next=v->fraction+v->sound->step;
            v->offset+=next>>16;v->fraction=next&65535UL;
        }
        mixed[frame*2]=clip(left);mixed[frame*2+1]=clip(right);
    }
}
static void pcm_update(void) {
#ifdef DOOM_GUEST
    unsigned budget = 4;
    if (!initialized) return;
    /* The speaker consumes wall time, not simulation ticks. In particular,
     * a burst of catch-up ticks must not generate seconds of queued effects. */
    while (budget-- && Doom_GuestAudioNeeded(PCM_FRAMES)) {
        mix_block();
        Doom_GuestAudioSubmit(mixed, PCM_FRAMES);
    }
#else
    uint32_t now=Doom_Tick();
    if(!initialized)return;
    while(mixed_tick<now) {
#if !defined(RIPPLE) && !defined(RCC_NATIVE) && !defined(DOOM_GUEST)
        unsigned int i;
        if(capture)for(i=0;i<PCM_FRAMES*2;++i) {
            unsigned short sample=(unsigned short)pending[i];
            if(fputc(sample&255U,capture)==EOF || fputc(sample>>8,capture)==EOF)I_Error("Native PCM capture failed");
        }
#endif
        mix_block();
#if defined(RIPPLE) || defined(RCC_NATIVE)
        if(vm_audio_submit(mixed,PCM_FRAMES)!=VM_DEVICE_OK)I_Error("PCM submission failed");
#else
        memcpy(pending,mixed,sizeof(pending));
#endif
        ++mixed_tick;
    }
#endif
}
/* Boot can advance a tick without drawing or calling S_UpdateSounds. Refill
 * before each grant; queue demand (guest) or mixed_tick guards duplicates. */
void Doom_PcmBeforeTick(void) { pcm_update(); }
static snddevice_t pcm_devices[]={SNDDEVICE_SB};
sound_module_t doom_pcm_module={pcm_devices,1,pcm_init,pcm_shutdown,pcm_lump,pcm_update,pcm_params,pcm_start,pcm_stop,pcm_playing,
#ifdef DOOM_GUEST
pcm_cache
#else
NULL
#endif
};
