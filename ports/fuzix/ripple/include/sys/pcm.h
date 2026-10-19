/* Signed 16-bit PCM: write() counts sample cells, not file-format octets. */
#ifndef RIPPLE_SYS_PCM_H
#define RIPPLE_SYS_PCM_H
#define PCM_VERSION 1
#define PCM_CONFIGURE 0x5100
#define PCM_STATE 0x5101
#define PCM_START 0x5102
#define PCM_PAUSE 0x5103
#define PCM_STOP 0x5104
#define PCM_GAIN 0x5105
#define PCM_END 0x5106
#define PCM_DRAIN 0x5107
#define PCM_OUTPUT_STATS 0x5108
#define PCM_MIX 0x5120
/* Optional stateless mono-octet -> stereo S16 mixer. All counts are cells;
 * resampling step/fraction are unsigned 16.16, gains are 0..127. */
struct pcm_mix_voice {
    const unsigned char *data;
    unsigned long length, offset, fraction, step;
    unsigned left, right;
};
struct pcm_mix {
    unsigned version, frames, count;
    struct pcm_mix_voice *voices;
    short *samples;
};
/* Optional OPL3/MUS synthesis on the same exclusive PCM lease (Fuse-X). */
#define PCM_MUSIC_BANK 0x5110
#define PCM_MUSIC_SCORE 0x5111
#define PCM_MUSIC_CONTROL 0x5112
#define PCM_MUSIC_STATE 0x5113
#define PCM_MUSIC_PLAY 1
#define PCM_MUSIC_STOP 2
#define PCM_MUSIC_PAUSE 3
#define PCM_MUSIC_RESUME 4
#define PCM_MUSIC_VOLUME 5
#define PCM_MUSIC_PLAYING 4
struct pcm_music_data { unsigned version; const unsigned char *data; unsigned long length; };
struct pcm_music_control { unsigned version, action, value; };
struct pcm_music_state { unsigned version, flags, volume; unsigned long long position; };
#define PCM_CONFIGURED 1
#define PCM_RUNNING 2
#define PCM_PAUSED 4
#define PCM_DRAINING 8
#define PCM_DONE 16
#define PCM_ERROR 32
struct pcm_format { unsigned version, channels; unsigned long rate, capacity; };
struct pcm_state {
    unsigned version, flags, channels;
    unsigned long rate, capacity, queued, position, underruns, late_ms;
};
/* Separate from logical underruns: counters from the physical speaker callback.
 * Reset by configure; an ending/pause silence is excluded. */
struct pcm_output_stats {
    unsigned version, live;
    unsigned long gaps, max_gap_ms;
    unsigned long long missing_frames;
};
#endif
