#ifndef VM_AUDIO_H
#define VM_AUDIO_H
#include <vm_devices.h>
#define VM_AUDIO_CONFIGURE 0x0200U
#define VM_AUDIO_SUBMIT 0x0201U
#define VM_AUDIO_STATE 0x0202U
#define VM_AUDIO_CONTROL 0x0203U
#define VM_AUDIO_RENDER 0x0204U
#define VM_AUDIO_OUTPUT_STATE 0x0205U
#define VM_AUDIO_OUTPUT_STATS 0x0206U
#define VM_AUDIO_MIX 0x0207U
typedef struct VmAudioMixVoice {
    const unsigned char *data;
    unsigned long length, offset, fraction, step;
    unsigned short left, right;
} VmAudioMixVoice;
unsigned short vm_audio_mix(VmAudioMixVoice *voices, unsigned short count, short *samples, unsigned short frames);
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
/* Ephemeral sink backlog, including submitted hardware buffers. Pure query. */
unsigned short vm_audio_output_state(unsigned long *pending_frames, unsigned short *live);
/* Physical output gaps, measured in host output frames since configure.
 * A gap is committed when new audio arrives, excluding startup/final silence. */
typedef struct VmAudioOutputStats {
    unsigned short live;
    unsigned long gaps, max_gap_ms;
    unsigned long long missing_frames;
} VmAudioOutputStats;
unsigned short vm_audio_output_stats(VmAudioOutputStats *stats);
/* Normally NEXT_TICK renders the next rational rate/hz frames. Programs which
 * do not use that clock can advance the same timeline explicitly. */
unsigned short vm_audio_render(unsigned long frames);
#endif
