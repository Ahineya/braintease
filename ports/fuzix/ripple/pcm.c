/* SPDX-License-Identifier: GPL-2.0-only */
/* One lease, a host-owned bounded sample queue, and kernel-local DMA staging.
 * Time is Unix monotonic time; this port never grants VM NEXT_TICK requests. */
#ifdef CONFIG_FUSE_X
#include "host.h"
#else
#include "platform.h"
#endif
#include <kdata.h>
#include <vm_audio.h>
#ifdef CONFIG_FUSE_X
#include <vm_music.h>
#include <native_process.h>
#endif
#include "include/sys/pcm.h"
#include "pcm.h"
static ptptr owner;
static unsigned owner_pid, configured, running, draining, done, failed, channels, fraction, waiting;
static unsigned long rate, last_ms, late_ms;
#ifdef CONFIG_FUSE_X
static unsigned char *mix_scratch;
static unsigned long mix_address(const void *p) {
    unsigned long v=(unsigned long)p;return (v>>16)*64000UL+(unsigned)v;
}
static int mix_overlap(const void *a,unsigned long n,const void *b,unsigned long m) {
    unsigned long x=mix_address(a),y=mix_address(b);return x<y+m&&y<x+n;
}
#endif
static int fail(unsigned error) { udata.u_error=error; return -1; }
static int mine(void) { return owner==udata.u_ptab && owner_pid==owner->p_pid; }
static void notify(void) { if(waiting)wakeup(&waiting);selwake_dev(5,0,SELECT_OUT); }
static int device(unsigned status) {
    if(status==VM_DEVICE_OK)return 0;
    return fail(status==VM_DEVICE_WOULD_BLOCK?EAGAIN:
        status==VM_DEVICE_UNSUPPORTED||status==VM_DEVICE_UNKNOWN_COMMAND?ENOSYS:
        status==VM_DEVICE_BAD_DESCRIPTOR||status==VM_DEVICE_BAD_STATE?EINVAL:EIO);
}
static void stop(void) {
#ifdef CONFIG_FUSE_X
    if(mix_scratch){rcc_process_kernel_free(mix_scratch);mix_scratch=NULL;}
#endif
    vm_audio_control(VM_AUDIO_RESET,0);
    configured=running=draining=done=failed=channels=fraction=0;rate=late_ms=0;
    notify();
}
void ripple_pcm_exit(ptptr process) {
    irqflags_t irq=di();
    if(owner==process){stop();owner=0;owner_pid=0;}
    irqrestore(irq);
}
int ripple_pcm_open(uint_fast8_t minor,uint16_t flags) {
    VmDeviceCaps caps;unsigned status;irqflags_t irq=di();
    if(minor){irqrestore(irq);return fail(ENXIO);}
    if(!O_ACCMODE(flags)){irqrestore(irq);return fail(EINVAL);}
    if(owner){irqrestore(irq);return fail(EBUSY);}
    status=vm_devices_caps(&caps);
    if(status!=VM_DEVICE_OK||!(caps.bits&VM_CAP_PCM)){irqrestore(irq);return fail(ENOSYS);}
    stop();owner=udata.u_ptab;owner_pid=owner->p_pid;
    irqrestore(irq);return 0;
}
int ripple_pcm_close(uint_fast8_t minor) {
    used(minor);
    if(owner&&mine())ripple_pcm_exit(owner);
    return 0;
}
void ripple_pcm_clock(const VmHostTime *time) {
    unsigned long now,elapsed,frames,total,host_frames;unsigned live,status;
    VmAudioState state;
    if(!owner||!configured||failed)return;
    now=time->seconds_low*1000UL+time->nanoseconds/1000000UL;
    elapsed=now-last_ms;last_ms=now;
    if(running&&elapsed) {
        if(elapsed>250UL){late_ms+=elapsed-250UL;elapsed=250UL;}
        total=rate*elapsed+fraction;frames=total/1000UL;fraction=total%1000UL;
        if(frames) {
            if(vm_audio_state(&state)!=VM_DEVICE_OK)goto error;
            if(draining&&frames>state.queued)frames=state.queued;
            if(frames&&vm_audio_render(frames)!=VM_DEVICE_OK)goto error;
            notify();
        }
    }
    if(draining) {
        if(vm_audio_state(&state)!=VM_DEVICE_OK)goto error;
        if(!state.queued) {
            running=0;
            status=vm_audio_output_state(&host_frames,&live);
            if(status!=VM_DEVICE_OK)goto error;
            if(!host_frames) {
                if(vm_audio_control(VM_AUDIO_PAUSE,0)!=VM_DEVICE_OK)goto error;
                draining=0;done=1;notify();
            }
        }
    }
    return;
error:failed=1;running=draining=0;notify();
}
unsigned ripple_pcm_status(void) {
    VmAudioState state;unsigned result=0;irqflags_t irq=di();
    if(!owner||!mine()||failed||!configured||draining||done)result=SELECT_OUT;
    else if(vm_audio_state(&state)!=VM_DEVICE_OK||state.queued<state.capacity)result=SELECT_OUT;
    irqrestore(irq);return result|SELECT_IN;
}
int ripple_pcm_write(uint_fast8_t minor,uint_fast8_t raw,uint_fast8_t flags) {
    short samples[2048];VmAudioState state;unsigned n,status;unsigned long frames;
    irqflags_t irq=di();used(raw);
    if(minor||!owner||!mine()){irqrestore(irq);return fail(EPERM);}
    if(!configured||failed||draining||done){irqrestore(irq);return fail(EIO);}
    if(udata.u_count%channels){irqrestore(irq);return fail(EINVAL);}
    if(!udata.u_count){irqrestore(irq);return 0;}
    for(;;) {
        if(device(vm_audio_state(&state))){irqrestore(irq);return -1;}
        frames=state.capacity-state.queued;
        if(frames)break;
        waiting=1;
        if(psleep_flags(&waiting,flags)){waiting=0;irqrestore(irq);return -1;}
        waiting=0;di();
        if(failed||!owner||!mine()){irqrestore(irq);return fail(EIO);}
    }
    if(frames>1024)frames=1024;
    if(frames>udata.u_count/channels)frames=udata.u_count/channels;
    n=(unsigned)frames*channels;
    if(udata.u_sysio)memcpy(samples,udata.u_base,n);
    else if(uget(udata.u_base,samples,n)){irqrestore(irq);return -1;}
    status=vm_audio_submit(samples,frames);
    irqrestore(irq);return device(status)?-1:n;
}
int ripple_pcm_ioctl(uint_fast8_t minor,uarg_t request,char *user) {
    struct pcm_format format;struct pcm_state out;VmAudioState state;VmHostTime now;
    unsigned value,status;int result=0;irqflags_t irq=di();
    if(minor||!owner||!mine()){irqrestore(irq);return fail(EPERM);}
    if(request==PCM_CONFIGURE) {
        if(uget(user,&format,sizeof(format))){irqrestore(irq);return -1;}
        if(format.version!=PCM_VERSION||format.channels<1||format.channels>2||format.rate<8000UL||format.rate>96000UL||
           !format.capacity||format.capacity>format.rate*2UL||format.capacity>192000UL){irqrestore(irq);return fail(EINVAL);}
        status=vm_audio_configure(format.rate,format.channels,format.capacity);
        if(device(status)){irqrestore(irq);return -1;}
        configured=1;rate=format.rate;channels=format.channels;fraction=running=draining=done=failed=0;late_ms=0;
    } else if(request==PCM_STATE) {
        if(device(vm_audio_state(&state))){irqrestore(irq);return -1;}
        memset(&out,0,sizeof(out));out.version=PCM_VERSION;out.channels=state.channels;
        out.flags=(configured?PCM_CONFIGURED:0)|(running?PCM_RUNNING:0)|
            (configured&&!running&&!done?PCM_PAUSED:0)|(draining?PCM_DRAINING:0)|(done?PCM_DONE:0)|(failed?PCM_ERROR:0);
        out.rate=state.rate;out.capacity=state.capacity;out.queued=state.queued;
        out.position=(unsigned long)state.position;out.underruns=state.underrun_frames;out.late_ms=late_ms;
        result=uput(&out,user,sizeof(out));
    } else if(request==PCM_OUTPUT_STATS) {
        VmAudioOutputStats stats;struct pcm_output_stats out;
        if(device(vm_audio_output_stats(&stats))){irqrestore(irq);return -1;}
        memset(&out,0,sizeof(out));out.version=PCM_VERSION;out.live=stats.live;
        out.gaps=stats.gaps;out.missing_frames=stats.missing_frames;out.max_gap_ms=stats.max_gap_ms;
        result=uput(&out,user,sizeof(out));
    } else if(request==PCM_GAIN) {
        if(uget(user,&value,sizeof(value))){irqrestore(irq);return -1;}
        result=device(vm_audio_control(VM_AUDIO_GAIN,value));
    } else if(request==PCM_STOP) {
        if(configured)result=device(vm_audio_control(VM_AUDIO_STOP,0));running=draining=done=fraction=0;notify();
    } else if(request==PCM_START) {
        if(!configured||failed||done){irqrestore(irq);return fail(EINVAL);}
        if(vm_host_monotonic(&now)!=VM_DEVICE_OK){irqrestore(irq);return fail(EIO);}
        last_ms=now.seconds_low*1000UL+now.nanoseconds/1000000UL;
        result=device(vm_audio_control(VM_AUDIO_START,0));if(!result)running=1;
    } else if(request==PCM_PAUSE) {
        result=device(vm_audio_control(VM_AUDIO_PAUSE,0));if(!result)running=0;
    } else if(request==PCM_END||request==PCM_DRAIN) {
        if(!configured||failed||(request==PCM_DRAIN&&!running&&!done&&!draining)){irqrestore(irq);return fail(EINVAL);}
        if(!done)draining=1;
        if(request==PCM_DRAIN)while(!done&&!failed) {
            waiting=1;if(psleep_flags(&waiting,0)){result=-1;break;}waiting=0;di();
        }
        waiting=0;if(failed)result=fail(EIO);
#ifdef CONFIG_FUSE_X
    } else if(request==PCM_MIX) {
        struct pcm_mix mix;
        struct pcm_mix_voice input[16];VmAudioMixVoice voices[16];short samples[1260];
        unsigned i,n;unsigned long needed,remaining;
        if(!configured||failed||channels!=2){irqrestore(irq);return fail(EINVAL);}
        if(uget(user,&mix,sizeof(mix))){irqrestore(irq);return -1;}
        if(mix.version!=PCM_VERSION||!mix.frames||mix.frames>630||!mix.count||mix.count>16){irqrestore(irq);return fail(EINVAL);}
        n=mix.count*sizeof(*input);
        if(!valaddr_w((void *)mix.voices,n,n)||!valaddr_w((void *)mix.samples,mix.frames*2,mix.frames*2)||uget(mix.voices,input,n)){irqrestore(irq);return -1;}
        if(mix_overlap(mix.voices,n,mix.samples,mix.frames*2UL)||mix_overlap(user,sizeof(mix),mix.voices,n)||mix_overlap(user,sizeof(mix),mix.samples,mix.frames*2UL)){irqrestore(irq);return fail(EINVAL);}
        for(i=0;i<mix.count;i++) {
            if(input[i].offset>=input[i].length||input[i].fraction>=65536UL||!input[i].step||input[i].step>196608UL||input[i].left>127||input[i].right>127){irqrestore(irq);return fail(EINVAL);}
            if(!valaddr_r(input[i].data,input[i].length,input[i].length)){irqrestore(irq);return -1;}
            if(mix_overlap(input[i].data,input[i].length,mix.samples,mix.frames*2UL)||mix_overlap(input[i].data,input[i].length,mix.voices,n)||mix_overlap(input[i].data,input[i].length,user,sizeof(mix))){irqrestore(irq);return fail(EINVAL);}
        }
        if(!mix_scratch)mix_scratch=rcc_process_kernel_alloc(16UL*2048UL);
        if(!mix_scratch){irqrestore(irq);return fail(ENOMEM);}
        for(i=0;i<mix.count;i++) {
            remaining=input[i].length-input[i].offset;
            needed=((input[i].fraction+(mix.frames-1UL)*input[i].step)>>16)+1UL;
            if(needed>remaining)needed=remaining;
            voices[i].data=mix_scratch+(unsigned long)i*2048UL;
            voices[i].length=needed;voices[i].offset=0;
            voices[i].fraction=input[i].fraction;voices[i].step=input[i].step;
            voices[i].left=input[i].left;voices[i].right=input[i].right;
            if(uget(input[i].data+input[i].offset,(void *)voices[i].data,needed)){irqrestore(irq);return -1;}
        }
        if(device(vm_audio_mix(voices,mix.count,samples,mix.frames))){irqrestore(irq);return -1;}
        for(i=0;i<mix.count;i++){input[i].offset+=voices[i].offset;input[i].fraction=voices[i].fraction;}
        result=uput(samples,mix.samples,mix.frames*2);
        if(!result)result=uput(input,mix.voices,n);
    } else if(request==PCM_MUSIC_BANK||request==PCM_MUSIC_SCORE) {
        struct pcm_music_data data;unsigned char *copy;
        if(!configured||failed){irqrestore(irq);return fail(EINVAL);}
        if(uget(user,&data,sizeof(data))){irqrestore(irq);return -1;}
        if(data.version!=PCM_VERSION||!data.length||data.length>65535UL||
           (request==PCM_MUSIC_BANK&&data.length!=VM_MUSIC_BANK_BYTES)) {irqrestore(irq);return fail(EINVAL);}
        copy=rcc_process_kernel_alloc(data.length);
        if(!copy){irqrestore(irq);return fail(ENOMEM);}
        if(uget(data.data,copy,data.length)){rcc_process_kernel_free(copy);irqrestore(irq);return -1;}
        status=request==PCM_MUSIC_BANK?vm_music_bank(copy,data.length):vm_music_score(copy,data.length);
        rcc_process_kernel_free(copy);result=device(status);
    } else if(request==PCM_MUSIC_CONTROL) {
        struct pcm_music_control control;
        if(!configured||failed){irqrestore(irq);return fail(EINVAL);}
        if(uget(user,&control,sizeof(control))){irqrestore(irq);return -1;}
        if(control.version!=PCM_VERSION){irqrestore(irq);return fail(EINVAL);}
        result=device(vm_music_control(control.action,control.value));
    } else if(request==PCM_MUSIC_STATE) {
        struct pcm_music_state out;VmMusicState state;
        if(!configured||failed){irqrestore(irq);return fail(EINVAL);}
        if(device(vm_music_state(&state))){irqrestore(irq);return -1;}
        memset(&out,0,sizeof(out));out.version=PCM_VERSION;out.flags=state.flags;out.volume=state.volume;out.position=state.position;
        result=uput(&out,user,sizeof(out));
#endif
    } else result=fail(ENOTTY);
    irqrestore(irq);return result;
}
