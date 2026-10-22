/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Real process/pipe/PCM acceptance. No scheduler or audio backend is faked. */
#include "SDL.h"
#include "SDL_mixer.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>

#define RATE 8000L
#define FRAMES 4096UL
#define OCTETS (FRAMES * 4UL)
#define HELPER "/bin/sdl-mixerd"
static unsigned checks, finished;
static char helper_env[] = "SDL_MIXER_HELPER=" HELPER;
static char missing_env[] = "SDL_MIXER_HELPER=/does-not-exist/sdl-mixerd";
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "SDL_MIXER_IPC_FAIL line %d: %s errno=%d (%s)\n", __LINE__, #c, errno, SDL_GetError()); exit(1); } } while (0)

static Uint32 ticks(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (Uint32)t.tv_sec * 1000UL + (Uint32)t.tv_nsec / 1000000UL;
}
static void rest(Uint32 ms)
{
    struct timeval delay;
    Uint32 start = ticks(), left;
    do {
        left = ms - (ticks() - start);
        delay.tv_sec = left / 1000UL; delay.tv_usec = (left % 1000UL) * 1000UL;
        if (select(0, NULL, NULL, NULL, &delay) == 0) return;
        CHECK(errno == EINTR);
    } while (ticks() - start < ms);
}
static int descriptors(void)
{
    int at, total = 0;
    for (at = 0; at < FD_SETSIZE; ++at) if (fcntl(at, F_GETFD, 0) >= 0) ++total;
    return total;
}
static void no_children(void)
{
    int status;
    errno = 0;
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}
static Uint8 *pattern(void)
{
    Uint8 *p = (Uint8 *)malloc(OCTETS * sizeof(Uint8));
    Uint32 i, n; Sint32 value;
    CHECK(p != NULL);
    for (i = 0; i < FRAMES; ++i) {
        value = (Sint32)((i * 37UL) % 251UL) * 83L - 10000L;
        n = (Uint32)value & 65535UL; p[i*4] = n & 255U; p[i*4+1] = n >> 8;
        value = (Sint32)((i * 53UL + 19UL) % 241UL) * 97L - 11000L;
        n = (Uint32)value & 65535UL; p[i*4+2] = n & 255U; p[i*4+3] = n >> 8;
    }
    return p;
}
static void opened(void)
{
    Sint32 rate; Uint16 format; int channels;
    CHECK(Mix_OpenAudio(RATE, AUDIO_S16LSB, 2, 64) == 0);
    CHECK(Mix_PCMHelperPID() > 0);
    CHECK(Mix_QuerySpec(&rate, &format, &channels) > 0);
    CHECK(rate == RATE && format == AUDIO_S16LSB && channels == 2);
}
static void notify(int channel) { CHECK(channel >= 0); ++finished; }
static void reopen_notify(int channel)
{
    CHECK(channel >= 0); ++finished;
    if (finished == 1) {
        Mix_CloseAudio();
        CHECK(Mix_OpenAudio(RATE, AUDIO_S16LSB, 2, 64) == 0);
    }
}
static void capture(const char *mode)
{
    Uint8 *data; Mix_Chunk *chunk;
    Uint32 start, elapsed, rounds = 0; volatile Uint32 work = 0x12345678UL;
    int p[2]; fd_set readset; struct timeval timeout;
    opened(); data = pattern(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
    CHECK(chunk->alen == OCTETS && Mix_PlayChannel(0, chunk, -1) == 0);
    /* Let queued initial silence drain before beginning the isolated work. */
    rest(180);
    start = ticks();
    if (!strcmp(mode, "block")) {
        CHECK(pipe(p) == 0); FD_ZERO(&readset); FD_SET(p[0], &readset);
        timeout.tv_sec = 0; timeout.tv_usec = 700000UL;
        CHECK(select(p[0]+1, &readset, NULL, NULL, &timeout) == 0);
        close(p[0]); close(p[1]);
    } else {
        CHECK(!strcmp(mode, "busy"));
        do {
            unsigned i;
            /* Real compiled guest work. No SDL call or I/O occurs here. */
            for (i = 0; i < 4096; ++i) {
                work ^= work << 13; work ^= work >> 17; work ^= work << 5;
            }
            ++rounds;
        } while (ticks() - start < 700UL);
    }
    elapsed = ticks() - start; CHECK(elapsed >= 650UL);
    CHECK(Mix_Playing(0) == 1); rest(180);
    Mix_HaltChannel(-1); Mix_FreeChunk(chunk); free(data); Mix_CloseAudio();
    no_children();
    printf("SDL_MIXER_IPC_CAPTURE mode=%s elapsed=%lu rounds=%lu work=%lu checks=%u\n",
           mode, (unsigned long)elapsed, (unsigned long)rounds, (unsigned long)work, checks);
}
static void controls(void)
{
    Uint8 *data = pattern(); Mix_Chunk *chunk; int before = descriptors(), iteration;
    for (iteration = 0; iteration < 3; ++iteration) {
        opened(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
        CHECK(Mix_AllocateChannels(257) == 257 && Mix_ReserveChannels(256) == 256);
        Mix_ChannelFinished(notify); finished = 0;
        CHECK(Mix_PlayChannel(-1, chunk, -1) == 256);
        CHECK(Mix_GetChunk(256) == chunk && Mix_Playing(-1) == 1);
        CHECK(Mix_Volume(256, 64) == 128 && Mix_Volume(256, -1) == 64);
        CHECK(Mix_VolumeChunk(chunk, 64) == 128 && Mix_VolumeChunk(chunk, -1) == 64);
        Mix_Pause(256); CHECK(Mix_Paused(256) == 1 && Mix_Playing(256) == 1);
        Mix_Resume(256); CHECK(Mix_Paused(256) == 0);
        CHECK(Mix_ExpireChannel(256, 40000L) == 1);
        CHECK(Mix_FadeOutChannel(256, 40000L) == 1 && Mix_FadingChannel(256) == MIX_FADING_OUT);
        CHECK(Mix_HaltChannel(256) == 0 && Mix_Playing(-1) == 0);
        SDL_Delay(40); CHECK(finished == 1);
        CHECK(Mix_PlayChannelTimed(0, chunk, 0, 20) == 0);
        rest(150); CHECK(Mix_Playing(0) == 0);
        SDL_Delay(40); CHECK(finished == 2);
        Mix_FreeChunk(chunk); Mix_ChannelFinished(NULL); Mix_CloseAudio();
        CHECK(Mix_PCMHelperPID() == 0 && descriptors() == before); no_children();
    }
    /* Closing/reopening in a completion callback creates a new session; a
     * second queued event from the old helper must not enter the new one. */
    opened(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk); finished = 0;
    Mix_ChannelFinished(reopen_notify);
    CHECK(Mix_PlayChannelTimed(0, chunk, -1, 20) == 0);
    CHECK(Mix_PlayChannelTimed(1, chunk, -1, 20) == 1);
    rest(150); SDL_Delay(80); CHECK(finished == 1);
    CHECK(Mix_PCMHelperPID() > 0 && Mix_Playing(-1) == 0);
    Mix_ChannelFinished(NULL); Mix_FreeChunk(chunk); Mix_CloseAudio();
    CHECK(descriptors() == before); no_children();

    /* SDL_Quit owns the optional backend's lifecycle even when the caller
     * never explicitly closes the mixer. It must reap and release its pipes. */
    opened(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
    CHECK(Mix_PlayChannel(0, chunk, -1) == 0); SDL_Quit();
    CHECK(Mix_PCMHelperPID() == 0 && descriptors() == before); no_children();
    Mix_FreeChunk(chunk); CHECK(SDL_Init(SDL_INIT_AUDIO) == 0);
    opened(); Mix_CloseAudio(); CHECK(descriptors() == before); no_children();
    free(data);
    printf("SDL_MIXER_IPC_CONTROLS passed checks=%u\n", checks);
}
static void late_poll(void)
{
    Uint8 *data; Mix_Chunk *chunk; Sint32 pid; int status, phase, before = descriptors();
    Uint32 start, elapsed;
    opened(); data = pattern(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
    finished = 0; Mix_ChannelFinished(notify);
    CHECK(Mix_PlayChannel(0, chunk, -1) == 0); pid = Mix_PCMHelperPID();
    for (phase = 0; phase < 2; ++phase) {
        /* Force a pending asynchronous request: an acknowledged stopped
         * server cannot answer the poll sent by this nonblocking boundary. */
        CHECK(kill((pid_t)pid, SIGSTOP) == 0);
        CHECK(waitpid((pid_t)pid, &status, WUNTRACED) == (pid_t)pid);
        CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
        SDL_PumpEvents(); SDL_PumpEvents(); CHECK(Mix_PCMHelperPID() == pid);
        CHECK(kill((pid_t)pid, SIGCONT) == 0);
        start = ticks(); rest(11000UL); elapsed = ticks() - start;
        CHECK(elapsed >= 10900UL);
        /* Exercise both synchronous RPC's pending-reply drain and the
         * nonblocking pump after the application has not run for >MX_TIMEOUT.
         * A reply already waiting in the pipe is healthy, however old it is. */
        if (phase) SDL_PumpEvents();
        CHECK(Mix_Playing(0) == 1 && Mix_PCMHelperPID() == pid && finished == 0);
        SDL_PumpEvents(); CHECK(Mix_PCMHelperPID() == pid);
        printf("SDL_MIXER_IPC_LATE_POLL phase=%d elapsed=%lu helper=%ld\n",
               phase, (unsigned long)elapsed, (long)pid);
    }
    Mix_ChannelFinished(NULL); Mix_FreeChunk(chunk); free(data); Mix_CloseAudio();
    CHECK(descriptors() == before); no_children();
    printf("SDL_MIXER_IPC_LATE_POLL passed checks=%u\n", checks);
}
static void failed_launch(void)
{
    int before = descriptors();
    CHECK(putenv(missing_env) == 0);
    CHECK(Mix_OpenAudio(RATE, AUDIO_S16LSB, 2, 64) < 0 && *SDL_GetError());
    Mix_CloseAudio(); CHECK(descriptors() == before); no_children();
    CHECK(putenv(helper_env) == 0);
    opened(); Mix_CloseAudio(); CHECK(descriptors() == before); no_children();
    printf("SDL_MIXER_IPC_FAILED_LAUNCH passed checks=%u\n", checks);
}
static void killed_helper(void)
{
    Uint8 *data; Mix_Chunk *chunk; int before = descriptors(); Sint32 pid;
    opened(); data = pattern(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
    CHECK(Mix_PlayChannel(0, chunk, -1) == 0); rest(100);
    pid = Mix_PCMHelperPID(); CHECK(pid > 0 && kill((pid_t)pid, SIGKILL) == 0);
    rest(100); SDL_ClearError(); CHECK(Mix_Playing(0) <= 0 && *SDL_GetError());
    Mix_CloseAudio(); Mix_FreeChunk(chunk); free(data);
    CHECK(descriptors() == before); no_children();
    opened(); Mix_CloseAudio(); CHECK(descriptors() == before); no_children();
    printf("SDL_MIXER_IPC_KILLED_HELPER passed checks=%u\n", checks);
}
static void abrupt(void)
{
    Uint8 *data; Mix_Chunk *chunk;
    opened(); data = pattern(); chunk = Mix_QuickLoad_RAW(data, OCTETS); CHECK(chunk);
    CHECK(Mix_PlayChannel(0, chunk, -1) == 0); rest(700);
    printf("SDL_MIXER_IPC_ABRUPT parent=%u helper=%ld\n", (unsigned)getpid(), (long)Mix_PCMHelperPID());
    fflush(stdout); _exit(0); /* Deliberately omit Mix_CloseAudio/SDL_Quit. */
}
static void probe(void)
{
    unsigned i;
    for (i = 0; i < 100; ++i) {
        if (Mix_OpenAudio(RATE, AUDIO_S16LSB, 2, 64) == 0) break;
        Mix_CloseAudio(); rest(20);
    }
    CHECK(i < 100); Mix_CloseAudio(); no_children();
    printf("SDL_MIXER_IPC_PROBE passed attempts=%u checks=%u\n", i+1, checks);
}

/* Exercise the server independently of the client: one logical octet per cell,
 * fragmented framing, a blocked reply pipe and uploads larger than its ring. */
static int request_fd, reply_fd; static pid_t helper_pid; static Uint32 sequence;
static void transfer(int fd, Uint8 *p, unsigned length, int writing)
{
    while (length) {
        int n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0); p += n; length -= (unsigned)n;
    }
}
static void answer(MX_Message *m, Uint32 expected, Uint32 operation)
{
    Uint8 header[MX_HEADER];
    transfer(reply_fd, header, MX_HEADER, 0); CHECK(mx_decode(m, header) == 0);
    CHECK(m->request == expected && m->operation == (operation | MX_REPLY));
    if (m->length) transfer(reply_fd, m->payload, (unsigned)m->length, 0);
    CHECK(m->arg[0] >= 0);
}
static void rpc(MX_Message *m)
{
    Uint8 header[MX_HEADER]; Uint32 id = ++sequence, operation = m->operation;
    m->request = id; mx_encode(header, m); transfer(request_fd, header, MX_HEADER, 1);
    if (m->length) transfer(request_fd, m->payload, (unsigned)m->length, 1);
    answer(m, id, operation);
}
static void message(MX_Message *m, Uint32 op)
{
    memset(m, 0, sizeof(*m)); m->operation = op;
}
static void raw_open(void)
{
    int input[2], output[2]; char r[16], w[16]; MX_Message m;
    char *args[] = { "sdl-mixerd", "--ipc", r, w, NULL };
    CHECK(pipe(input) == 0 && pipe(output) == 0); helper_pid = fork(); CHECK(helper_pid >= 0);
    if (!helper_pid) {
        close(input[1]); close(output[0]);
        snprintf(r, sizeof(r), "%d", input[0]); snprintf(w, sizeof(w), "%d", output[1]);
        execv(HELPER, args); _exit(127);
    }
    close(input[0]); close(output[1]); request_fd = input[1]; reply_fd = output[0]; sequence = 0;
    message(&m, MX_OPEN); m.arg[0] = RATE; m.arg[1] = AUDIO_S16LSB; m.arg[2] = 2; m.arg[3] = 64; rpc(&m);
}
static void raw_upload(void)
{
    Uint8 *data = pattern(); Uint32 at, take, i; MX_Message m;
    message(&m, MX_BEGIN); m.arg[0] = 1; m.arg[1] = OCTETS; m.arg[2] = 128; rpc(&m);
    for (at = 0; at < OCTETS; at += take) {
        take = OCTETS - at; if (take > MX_PAYLOAD) take = MX_PAYLOAD;
        message(&m, MX_DATA); m.arg[0] = 1; m.arg[1] = at; m.length = take;
        for (i = 0; i < take; ++i) m.payload[i] = data[at+i]; rpc(&m);
    }
    free(data); message(&m, MX_COMMIT); m.arg[0] = 1; rpc(&m);
    message(&m, MX_PLAY); m.arg[0] = 0; m.arg[1] = 1; m.arg[2] = -1; m.arg[3] = -1; m.arg[4] = -1; rpc(&m);
}
static void raw(const char *mode)
{
    MX_Message m; Uint8 header[MX_HEADER]; Uint32 start, bytes = 0, retries = 0;
    int status, offset = 0;
    raw_open(); raw_upload(); rest(180);
    if (!strcmp(mode, "partial")) {
        message(&m, MX_POLL); m.request = ++sequence; mx_encode(header, &m);
        transfer(request_fd, header, 13, 1); rest(700);
        transfer(request_fd, header+13, MX_HEADER-13, 1); answer(&m, sequence, MX_POLL);
        rest(180); message(&m, MX_QUIT); rpc(&m); close(request_fd); close(reply_fd);
    } else {
        CHECK(!strcmp(mode, "flood"));
        CHECK(fcntl(request_fd, F_SETFL, O_NONBLOCK) == 0);
        start = ticks();
        while (ticks() - start < 350UL) {
            int n;
            if (!offset) { message(&m, MX_POLL); m.request = ++sequence; mx_encode(header, &m); }
            n = write(request_fd, header + offset, MX_HEADER - offset);
            if (n < 0) {
                CHECK(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                ++retries; rest(2); continue;
            }
            CHECK(n > 0); bytes += (unsigned)n; offset = (offset+n) % MX_HEADER;
        }
        CHECK(bytes > 8192UL && retries > 0); rest(700);
        /* Deliberately abandon unread replies; the helper must notice loss of
         * its consumer, release PCM and exit instead of hanging on a write. */
        close(request_fd); close(reply_fd);
    }
    CHECK(waitpid(helper_pid, &status, 0) == helper_pid);
    CHECK(!strcmp(mode, "flood") || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
    no_children();
    printf("SDL_MIXER_IPC_RAW mode=%s bytes=%lu retries=%lu checks=%u\n", mode,
           (unsigned long)bytes, (unsigned long)retries, checks);
}
int main(int argc, char **argv)
{
    CHECK(SDL_Init(SDL_INIT_AUDIO) == 0);
    CHECK(putenv(helper_env) == 0);
    if (argc == 3 && !strcmp(argv[1], "--capture")) capture(argv[2]);
    else if (argc == 3 && !strcmp(argv[1], "--raw")) raw(argv[2]);
    else if (argc == 2 && !strcmp(argv[1], "--controls")) controls();
    else if (argc == 2 && !strcmp(argv[1], "--late-poll")) late_poll();
    else if (argc == 2 && !strcmp(argv[1], "--failed-launch")) failed_launch();
    else if (argc == 2 && !strcmp(argv[1], "--kill-helper")) killed_helper();
    else if (argc == 2 && !strcmp(argv[1], "--abrupt")) abrupt();
    else if (argc == 2 && !strcmp(argv[1], "--probe")) probe();
    else { fprintf(stderr, "usage: sdl-mixer-ipc --capture block|busy | --raw partial|flood | --controls | --late-poll | --failed-launch | --kill-helper | --abrupt | --probe\n"); return 2; }
    SDL_Quit(); return 0;
}
