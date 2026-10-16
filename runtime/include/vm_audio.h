#ifndef VM_AUDIO_H
#define VM_AUDIO_H
#include <vm_devices.h>
#define VM_AUDIO_CONFIGURE 0x0200U
#define VM_AUDIO_SUBMIT 0x0201U
#define VM_AUDIO_STATE 0x0202U
#define VM_AUDIO_CONTROL 0x0203U
#define VM_AUDIO_RENDER 0x0204U
#define VM_AUDIO_START 1
#define VM_AUDIO_PAUSE 2
#define VM_AUDIO_STOP 3
#define VM_AUDIO_RESET 4
#define VM_AUDIO_GAIN 5
#define VM_AUDIO_CONFIGURED 1
#define VM_AUDIO_RUNNING 2
#define VM_AUDIO_UNDERRUN 4
#define VM_AUDIO_ERROR 8
/* Signed 16-bit interleaved PCM: one sample per cell. Submission copies all
 * frames or returns WOULD_BLOCK; it never retains a pointer into guest RAM. */
typedef struct VmAudioState {
    unsigned short flags, channels;
    unsigned long rate, capacity, queued, underrun_frames;
    unsigned long long position;
} VmAudioState;
unsigned short vm_audio_configure(unsigned long rate, unsigned short channels, unsigned long capacity);
unsigned short vm_audio_submit(const short *samples, unsigned long frames);
unsigned short vm_audio_control(unsigned short action, unsigned short value);
unsigned short vm_audio_state(VmAudioState *state);
/* Normally NEXT_TICK renders the next rational rate/hz frames. Programs which
 * do not use that clock can advance the same timeline explicitly. */
unsigned short vm_audio_render(unsigned long frames);
#endif
