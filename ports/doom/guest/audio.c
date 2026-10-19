/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/pcm.h>
#include <unistd.h>
#include "audio.h"
#include "i_system.h"

static int pcm = -1;
static int requested, started;
static unsigned block_frames;
static unsigned long dropped;
static short silence[1260];
static int mixer = 1;
int Doom_GuestAudioMix(struct pcm_mix_voice *voices,unsigned count,short *samples,unsigned frames) {
    struct pcm_mix request;
    if (pcm < 0 || !mixer) return 0;
    request.version = PCM_VERSION; request.voices = voices; request.count = count;
    request.samples = samples; request.frames = frames;
    if (!ioctl(pcm, PCM_MIX, &request)) return 1;
    if (errno == ENOTTY || errno == ENOSYS) { mixer = 0; return 0; }
    I_Error("Cannot mix guest sound effects"); return 0;
}
int Doom_GuestAudioFd(void) { return pcm; }
unsigned long Doom_GuestAudioDropped(void) { return dropped; }
int Doom_GuestAudioNeeded(unsigned frames) {
    struct pcm_state state;
    if (pcm < 0 || !started) return 0;
    if (ioctl(pcm, PCM_STATE, &state)) I_Error("Cannot read guest audio queue");
    return state.queued + frames <= (unsigned long)block_frames * 4UL;
}
void Doom_GuestAudioClose(void) {
    if (pcm >= 0) { ioctl(pcm, PCM_STOP, 0); close(pcm); pcm = -1; }
    requested = started = 0;
}
int Doom_GuestAudioOpen(unsigned long rate, unsigned frames) {
    struct pcm_format format;
    if (pcm >= 0) return 1;
    pcm = open("/dev/pcm", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (pcm < 0) { puts("Doom audio unavailable"); return 0; }
    format.version = PCM_VERSION; format.channels = 2;
    /* Interactive effects need a short bounded lead, unlike a file player. */
    format.rate = rate; format.capacity = (unsigned long)frames * 6UL;
    if (ioctl(pcm, PCM_CONFIGURE, &format)) { Doom_GuestAudioClose(); return 0; }
    atexit(Doom_GuestAudioClose);
    block_frames = frames;
    dropped = 0;
    return 1;
}
void Doom_GuestAudioStart(void) {
    /* Initialization can load a whole level. Start after that work has
     * produced the first frame, with a small lead for scheduling jitter. */
    requested = 1;
}
void Doom_GuestAudioSubmit(const short *samples, unsigned frames) {
    unsigned left = frames * 2U;
    while (left && pcm >= 0) {
        int count = write(pcm, samples, left);
        if (count > 0) { samples += count; left -= count; }
        /* Stale effects may be dropped under load; never stall gameplay to
         * wait for a full sound queue (including unthrottled demo runs). */
        else if (count < 0 && errno == EAGAIN) { dropped += left / 2U; return; }
        else if (count < 0 && errno == EINTR) continue;
        else I_Error("Cannot submit guest audio");
    }
}

void Doom_GuestAudioPump(void) {
    struct pcm_state state;
    unsigned budget = 4;
    if (pcm < 0 || !requested) return;
    if (ioctl(pcm, PCM_STATE, &state)) I_Error("Cannot read guest audio queue");
    /* Refill a lost lead with silence rather than replaying old effects.
     * OPL music has its independent wall-clock timeline in the same stream. */
    if (!started || state.queued < (unsigned long)block_frames * 3UL) {
        while (state.queued < (unsigned long)block_frames * 4UL && budget--) {
            unsigned frames = (unsigned)((unsigned long)block_frames * 4UL - state.queued);
            if (frames > block_frames) frames = block_frames;
            Doom_GuestAudioSubmit(silence, frames);
            state.queued += frames;
        }
    }
    if (!started) {
        if (ioctl(pcm, PCM_START, 0)) I_Error("Cannot start guest audio");
        started = 1;
    }
}
