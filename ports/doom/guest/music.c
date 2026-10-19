/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/pcm.h>
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"
#include "audio.h"

boolean Doom_PcmEnsure(void);
static boolean music_init(void) {
    struct pcm_music_data bank;
    int lump;
    if (!Doom_PcmEnsure()) return false;
    lump = W_GetNumForName("GENMIDI");
    if (W_LumpLength(lump) < 6308UL) I_Error("Truncated GENMIDI bank");
    bank.version = PCM_VERSION; bank.length = 6308UL;
    bank.data = W_CacheLumpNum(lump, PU_STATIC);
    if (ioctl(Doom_GuestAudioFd(), PCM_MUSIC_BANK, &bank)) {
        perror("Doom music unavailable");
        W_ReleaseLumpNum(lump);
        return false;
    }
    W_ReleaseLumpNum(lump);
    puts("Doom OPL3 music enabled");
    return true;
}
static void control(unsigned action, unsigned value) {
    struct pcm_music_control c;
    c.version = PCM_VERSION; c.action = action; c.value = value;
    if (Doom_GuestAudioFd() >= 0 && ioctl(Doom_GuestAudioFd(), PCM_MUSIC_CONTROL, &c))
        I_Error("Guest music control failed");
}
static void music_stop(void) { control(PCM_MUSIC_STOP, 0); }
static void music_volume(int volume) { if (volume < 0) volume = 0; if (volume > 127) volume = 127; control(PCM_MUSIC_VOLUME, volume); }
static void music_pause(void) { control(PCM_MUSIC_PAUSE, 0); }
static void music_resume(void) { control(PCM_MUSIC_RESUME, 0); }
static void *music_register(void *data, int length) {
    struct pcm_music_data score;
    score.version = PCM_VERSION; score.data = data; score.length = (unsigned)length;
    if (score.length < 16 || ioctl(Doom_GuestAudioFd(), PCM_MUSIC_SCORE, &score))
        I_Error("Invalid or unsupported MUS score");
    return data;
}
static void music_unregister(void *handle) { (void)handle; music_stop(); }
static void music_play(void *handle, boolean looping) { if (handle) control(PCM_MUSIC_PLAY, looping ? 1 : 0); }
static boolean music_playing(void) {
    struct pcm_music_state state;
    if (ioctl(Doom_GuestAudioFd(), PCM_MUSIC_STATE, &state)) I_Error("Cannot read guest music state");
    return (state.flags & PCM_MUSIC_PLAYING) != 0;
}
static snddevice_t devices[] = { SNDDEVICE_SB, SNDDEVICE_ADLIB };
music_module_t doom_music_module = { devices, 2, music_init, music_stop, music_volume, music_pause, music_resume,
    music_register, music_unregister, music_play, music_stop, music_playing, NULL };
