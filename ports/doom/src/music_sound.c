/* Host OPL3 music. Only normal Ripple/RCC-native builds include this module;
 * the Clang pixel/SFX oracle has no VM music device. */
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"
#if defined(RIPPLE) || defined(RCC_NATIVE)
#include <vm_music.h>
boolean Doom_PcmEnsure(void);
static boolean music_init(void) {
    VmDeviceCaps caps;
    int lump;
    byte *bank;
    unsigned short status;
    if(vm_devices_caps(&caps)!=VM_DEVICE_OK || !(caps.bits&VM_CAP_MUSIC) || !Doom_PcmEnsure())return false;
    lump=W_GetNumForName("GENMIDI");
    if(W_LumpLength((unsigned int)lump)<VM_MUSIC_BANK_BYTES)I_Error("Truncated GENMIDI bank");
    bank=W_CacheLumpNum(lump,PU_STATIC);
    status=vm_music_bank(bank,VM_MUSIC_BANK_BYTES);
    W_ReleaseLumpNum(lump);
    if(status!=VM_DEVICE_OK)I_Error("Cannot load GENMIDI bank");
    return true;
}
static void music_control(unsigned short action,unsigned short value) {
    if(vm_music_control(action,value)!=VM_DEVICE_OK)I_Error("VM music control failed");
}
static void music_stop(void){music_control(VM_MUSIC_STOP,0);}
static void music_volume(int volume){if(volume<0)volume=0;if(volume>127)volume=127;music_control(VM_MUSIC_VOLUME,(unsigned short)volume);}
static void music_pause(void){music_control(VM_MUSIC_PAUSE,0);}
static void music_resume(void){music_control(VM_MUSIC_RESUME,0);}
static void *music_register(void *data,int length) {
    /* The engine ABI passes a 16-bit int. IWAD tracks can exceed 32767 octets;
     * preserve its unsigned bit pattern (all supported IWAD tracks fit u16). */
    unsigned long bytes=(unsigned int)length;
    if(bytes<16UL || vm_music_score((const byte *)data,bytes)!=VM_DEVICE_OK)I_Error("Invalid or unsupported MUS score");
    return data;
}
static void music_unregister(void *handle){(void)handle;music_stop();}
static void music_play(void *handle,boolean looping){if(handle)music_control(VM_MUSIC_PLAY,looping?1:0);}
static boolean music_playing(void){VmMusicState state;if(vm_music_state(&state)!=VM_DEVICE_OK)I_Error("Cannot read music state");return (state.flags&VM_MUSIC_PLAYING)!=0;}
static snddevice_t music_devices[]={SNDDEVICE_SB,SNDDEVICE_ADLIB};
music_module_t doom_music_module={music_devices,2,music_init,music_stop,music_volume,music_pause,music_resume,music_register,music_unregister,music_play,music_stop,music_playing,NULL};
#else
/* The independent Clang reference still supplies pixel and PCM SFX oracles. */
static boolean music_init(void){return false;}
music_module_t doom_music_module={NULL,0,music_init,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL};
#endif
