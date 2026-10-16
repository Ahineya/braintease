#ifndef VM_MUSIC_H
#define VM_MUSIC_H
#include <vm_audio.h>
#define VM_MUSIC_BANK 0x0210U
#define VM_MUSIC_SCORE 0x0211U
#define VM_MUSIC_CONTROL 0x0212U
#define VM_MUSIC_STATE 0x0213U
#define VM_MUSIC_PLAY 1
#define VM_MUSIC_STOP 2
#define VM_MUSIC_PAUSE 3
#define VM_MUSIC_RESUME 4
#define VM_MUSIC_VOLUME 5
#define VM_MUSIC_HAS_BANK 1
#define VM_MUSIC_HAS_SCORE 2
#define VM_MUSIC_PLAYING 4
#define VM_MUSIC_PAUSED 8
#define VM_MUSIC_LOOPING 16
#define VM_MUSIC_BANK_BYTES 6308UL
/* One octet per cell, copied on submission. Music shares the configured PCM
 * clock/output; pause freezes its score while PCM effects keep playing. */
typedef struct VmMusicState {
    unsigned short flags, volume;
    unsigned long long position;
} VmMusicState;
unsigned short vm_music_bank(const unsigned char *data, unsigned long length);
unsigned short vm_music_score(const unsigned char *data, unsigned long length);
unsigned short vm_music_control(unsigned short action, unsigned short value);
unsigned short vm_music_state(VmMusicState *state);
#endif
