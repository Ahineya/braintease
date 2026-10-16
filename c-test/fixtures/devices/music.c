#include <stdio.h>
#include <stdlib.h>
#include <vm_music.h>
static unsigned char bank[6308];
static const unsigned char score[]={ 'M','U','S',26,4,0,16,0,1,0,0,0,0,0,0,0,0x90,60,14,0x60 };
static void check(int condition){if(!condition){puts("music device failed");exit(1);}}
int main(void) {
    VmDeviceCaps caps;
    VmMusicState state;
    unsigned int i;
    const char *header="#OPL_II#";
    for(i=0;i<8;++i)bank[i]=(unsigned char)header[i];
    bank[12]=bank[19]=0x21;bank[13]=bank[20]=0xf2;bank[14]=bank[21]=0x24;bank[17]=32;
    check(vm_devices_caps(&caps)==VM_DEVICE_OK && (caps.bits&VM_CAP_MUSIC));
    check(vm_audio_configure(8000,2,1000)==VM_DEVICE_OK);
    check(vm_music_control(VM_MUSIC_PLAY,0)==VM_DEVICE_BAD_STATE);
    check(vm_music_bank(bank,6308)==VM_DEVICE_OK);
    check(vm_music_score(score,sizeof(score))==VM_DEVICE_OK);
    check(vm_music_control(VM_MUSIC_PLAY,1)==VM_DEVICE_OK);
    check(vm_audio_control(VM_AUDIO_START,0)==VM_DEVICE_OK);
    check(vm_audio_render(160)==VM_DEVICE_OK);
    check(vm_music_state(&state)==VM_DEVICE_OK && state.position==160 && (state.flags&VM_MUSIC_PLAYING));
    check(vm_music_control(VM_MUSIC_PAUSE,0)==VM_DEVICE_OK);
    check(vm_audio_render(80)==VM_DEVICE_OK);
    check(vm_music_state(&state)==VM_DEVICE_OK && state.position==160 && (state.flags&VM_MUSIC_PAUSED));
    check(vm_music_control(VM_MUSIC_RESUME,0)==VM_DEVICE_OK);
    check(vm_audio_render(640)==VM_DEVICE_OK);
    check(vm_music_state(&state)==VM_DEVICE_OK && state.position==0);
    check(vm_music_control(VM_MUSIC_VOLUME,0)==VM_DEVICE_OK);
    check(vm_audio_render(64)==VM_DEVICE_OK);
    check(vm_music_control(VM_MUSIC_VOLUME,128)==VM_DEVICE_BAD_DESCRIPTOR);
    check(vm_music_control(VM_MUSIC_STOP,0)==VM_DEVICE_OK);
    check(vm_audio_render(64)==VM_DEVICE_OK);
    check(vm_music_state(&state)==VM_DEVICE_OK && state.position==0 && !(state.flags&VM_MUSIC_PLAYING));
    puts("music device ok");return 0;
}
