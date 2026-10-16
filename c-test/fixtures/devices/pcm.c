#include <vm_audio.h>
#include <stdio.h>
static short first[4]={-32768,0,32767,-1};
static short second[2]={32767,-32768};
int main(void) {
    VmDeviceCaps caps;VmAudioState state;VmClock clock;
    if(vm_devices_caps(&caps)!=VM_DEVICE_OK || !(caps.bits&VM_CAP_PCM))return 1;
    if(vm_audio_configure(8000UL,1,8UL)!=VM_DEVICE_OK)return 2;
    if(vm_audio_submit(first,4UL)!=VM_DEVICE_OK)return 3;
    first[0]=111; /* Submitted cells have already been copied. */
    if(vm_audio_control(VM_AUDIO_START,0)!=VM_DEVICE_OK)return 4;
    if(vm_audio_render(4UL)!=VM_DEVICE_OK)return 5;
    if(vm_audio_control(VM_AUDIO_GAIN,32768U)!=VM_DEVICE_OK)return 6;
    if(vm_audio_submit(second,2UL)!=VM_DEVICE_OK || vm_audio_render(2UL)!=VM_DEVICE_OK)return 7;
    if(vm_audio_render(2UL)!=VM_DEVICE_OK)return 8;
    if(vm_audio_state(&state)!=VM_DEVICE_OK || state.position!=8ULL || state.queued || state.underrun_frames!=2UL || state.channels!=1 || state.rate!=8000UL)return 9;
    if(vm_audio_control(VM_AUDIO_PAUSE,0)!=VM_DEVICE_OK || vm_next_tick(&clock)!=VM_DEVICE_OK)return 10;
    if(vm_audio_state(&state)!=VM_DEVICE_OK || state.position!=8ULL)return 11;
    if(vm_audio_render(1UL)!=VM_DEVICE_BAD_STATE)return 12;
    if(vm_audio_configure(8000UL,3,8UL)!=VM_DEVICE_BAD_DESCRIPTOR)return 13;
    puts("pcm device ok");return 0;
}
