/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "SDL.h"
#include "audio.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/pcm.h>
#include <unistd.h>

static int pcm_fd = -1;
static unsigned pcm_channels;

static int error(const char *operation) {
    SDL_SetError("PCM %s failed (errno %d)", operation, errno);
    return -1;
}

int SDL_PCMOpen(Sint32 rate, Uint8 channels, Uint32 capacity) {
    struct pcm_format format;
    if (pcm_fd >= 0) {
        SDL_SetError("PCM device is already open");
        return -1;
    }
    pcm_fd = open("/dev/pcm", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (pcm_fd < 0) return error("open");
    SDL_memset(&format, 0, sizeof(format));
    format.version = PCM_VERSION;
    format.rate = rate;
    format.channels = channels;
    format.capacity = capacity;
    if (ioctl(pcm_fd, PCM_CONFIGURE, &format) < 0) {
        error("configure");
        close(pcm_fd);
        pcm_fd = -1;
        return -1;
    }
    pcm_channels = channels;
    return 0;
}

void SDL_PCMClose(void) {
    if (pcm_fd >= 0) close(pcm_fd);
    pcm_fd = -1;
    pcm_channels = 0;
}

int SDL_PCMState(Uint32 *queued, Uint32 *capacity) {
    struct pcm_state state;
    SDL_memset(&state, 0, sizeof(state));
    state.version = PCM_VERSION;
    if (ioctl(pcm_fd, PCM_STATE, &state) < 0) return error("state");
    if (state.flags & PCM_ERROR || !(state.flags & PCM_CONFIGURED) ||
        state.queued > state.capacity || state.channels != pcm_channels) {
        SDL_SetError("PCM device returned an invalid playback state");
        return -1;
    }
    *queued = state.queued;
    *capacity = state.capacity;
    return 0;
}

Sint32 SDL_PCMWrite(const Sint16 *samples, Uint32 frames) {
    int count;
    /* The PCM driver accepts at most 1024 frames and counts sample cells. */
    if (frames > 1024) frames = 1024;
    count = write(pcm_fd, samples, frames * pcm_channels);
    if (count < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        return error("write");
    }
    if ((unsigned)count % pcm_channels) {
        SDL_SetError("PCM returned a partial interleaved frame");
        return -1;
    }
    return (unsigned)count / pcm_channels;
}

int SDL_PCMStart(void) {
    return ioctl(pcm_fd, PCM_START, NULL) < 0 ? error("start") : 0;
}
int SDL_PCMPause(void) {
    return ioctl(pcm_fd, PCM_PAUSE, NULL) < 0 ? error("pause") : 0;
}
