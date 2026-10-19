#include <vm_audio.h>
#include <mmio.h>
static unsigned short submit_audio(unsigned short command, volatile unsigned short *desc) {
    unsigned long p = (unsigned long)desc;
    mmio_write(MMIO_DEVICE_DESC_BANK,(unsigned short)(p >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF,(unsigned short)p);
    mmio_write(MMIO_DEVICE_COMMAND,command);
    return mmio_read(MMIO_DEVICE_STATUS);
}
static unsigned long audio_words(unsigned short lo,unsigned short hi) { return (unsigned long)lo | ((unsigned long)hi << 16); }
unsigned short vm_audio_mix(VmAudioMixVoice *voices,unsigned short count,short *samples,unsigned short frames) {
    volatile unsigned short d[10];
    unsigned long v=(unsigned long)voices,p=(unsigned long)samples;
    if(sizeof(VmAudioMixVoice)!=12)return VM_DEVICE_BAD_DESCRIPTOR;
    d[0]=1;d[1]=10;d[2]=v>>16;d[3]=(unsigned short)v;d[4]=count;
    d[5]=p>>16;d[6]=(unsigned short)p;d[7]=frames;d[8]=d[9]=0;
    return submit_audio(VM_AUDIO_MIX,d);
}
unsigned short vm_audio_output_stats(VmAudioOutputStats *out) {
    volatile unsigned short d[12];unsigned short status,i;
    d[0]=1;d[1]=12;for(i=2;i<12;i++)d[i]=0;
    status=submit_audio(VM_AUDIO_OUTPUT_STATS,d);
    if(status==VM_DEVICE_OK){
        out->live=d[2];out->gaps=audio_words(d[4],d[5]);
        out->missing_frames=(unsigned long long)audio_words(d[6],d[7]) |
            ((unsigned long long)audio_words(d[8],d[9]) << 32);
        out->max_gap_ms=audio_words(d[10],d[11]);
    }
    return status;
}
unsigned short vm_audio_output_state(unsigned long *pending,unsigned short *live) {
    volatile unsigned short d[6];unsigned short status,i;
    d[0]=1;d[1]=6;for(i=2;i<6;i++)d[i]=0;
    status=submit_audio(VM_AUDIO_OUTPUT_STATE,d);
    if(status==VM_DEVICE_OK){*pending=audio_words(d[2],d[3]);*live=d[4];}
    return status;
}
unsigned short vm_audio_configure(unsigned long rate,unsigned short channels,unsigned long capacity) {
    volatile unsigned short d[8];
    d[0]=1;d[1]=8;d[2]=(unsigned short)rate;d[3]=(unsigned short)(rate >> 16);
    d[4]=channels;d[5]=(unsigned short)capacity;d[6]=(unsigned short)(capacity >> 16);d[7]=0;
    return submit_audio(VM_AUDIO_CONFIGURE,d);
}
unsigned short vm_audio_submit(const short *samples,unsigned long frames) {
    volatile unsigned short d[10];
    unsigned long p=(unsigned long)samples;
    d[0]=1;d[1]=10;d[2]=(unsigned short)(p >> 16);d[3]=(unsigned short)p;
    d[4]=(unsigned short)frames;d[5]=(unsigned short)(frames >> 16);d[6]=d[7]=d[8]=d[9]=0;
    return submit_audio(VM_AUDIO_SUBMIT,d);
}
unsigned short vm_audio_control(unsigned short action,unsigned short value) {
    volatile unsigned short d[4];d[0]=1;d[1]=4;d[2]=action;d[3]=value;
    return submit_audio(VM_AUDIO_CONTROL,d);
}
unsigned short vm_audio_render(unsigned long frames) {
    volatile unsigned short d[6];d[0]=1;d[1]=6;d[2]=(unsigned short)frames;d[3]=(unsigned short)(frames >> 16);d[4]=d[5]=0;
    return submit_audio(VM_AUDIO_RENDER,d);
}
unsigned short vm_audio_state(VmAudioState *state) {
    volatile unsigned short d[16];unsigned short status,i;
    d[0]=1;d[1]=16;for(i=2;i<16;++i)d[i]=0;
    status=submit_audio(VM_AUDIO_STATE,d);
    if(status==VM_DEVICE_OK) {
        state->flags=d[2];state->channels=d[3];state->rate=audio_words(d[4],d[5]);
        state->capacity=audio_words(d[6],d[7]);state->queued=audio_words(d[8],d[9]);
        state->position=(unsigned long long)audio_words(d[10],d[11]) | ((unsigned long long)audio_words(d[12],d[13]) << 32);
        state->underrun_frames=audio_words(d[14],d[15]);
    }
    return status;
}
