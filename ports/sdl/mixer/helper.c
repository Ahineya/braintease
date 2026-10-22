/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A private PCM producer process. The application sends values and copied
 * sample octets; no application pointer or callback crosses this boundary.
 * All transport I/O is nonblocking, including replies to a stalled client. */
#include "SDL_mixer.h"
#include "ipc.h"
#include "audio.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

typedef struct Chunk_Record {
    struct Chunk_Record *next;
    Sint32 id;
    Uint32 length, received;
    int volume;
    Uint8 *data;
    Mix_Chunk *chunk;
} Chunk_Record;

static Chunk_Record *chunks;
static Uint8 *completed;
static int completed_capacity, audio_open;

static void SDLCALL channel_finished(int channel)
{
    if (channel >= 0 && channel < completed_capacity)
        completed[channel] = 1;
}

/* Retain flags for removed channels until POLL acknowledges them. The mixer
 * can report completions while shrinking its channel array. */
static int grow_completed(int count)
{
    Uint8 *next;
    if (count <= completed_capacity) return 0;
    next = (Uint8 *)SDL_calloc((Uint32)count, sizeof(*next));
    if (!next) { SDL_OutOfMemory(); return -1; }
    if (completed_capacity)
        SDL_memcpy(next, completed, (Uint32)completed_capacity * sizeof(*next));
    SDL_free(completed);
    completed = next;
    completed_capacity = count;
    return 0;
}

static Chunk_Record *find_chunk(Sint32 id)
{
    Chunk_Record *record;
    for (record = chunks; record; record = record->next)
        if (record->id == id) return record;
    return NULL;
}

static void free_record(Chunk_Record *record)
{
    if (record->chunk) Mix_FreeChunk(record->chunk);
    else SDL_free(record->data);
    SDL_free(record);
}

static void cleanup(void)
{
    Chunk_Record *record;
    Mix_ChannelFinished(NULL);
    if (audio_open) Mix_CloseAudio();
    audio_open = 0;
    while (chunks) {
        record = chunks;
        chunks = record->next;
        free_record(record);
    }
    SDL_free(completed);
    completed = NULL;
    completed_capacity = 0;
}

static int native_int(Sint32 value, Sint32 low, Sint32 high)
{
    if (value >= low && value <= high) return 1;
    SDL_SetError("Mixer IPC argument is outside its public range");
    return 0;
}

static int channel_arg(Sint32 value, int all)
{
    return native_int(value, all ? -1 : 0, MIX_PCM_MAX_CHANNELS - 1);
}

static int known_audio(void)
{
    if (audio_open) return 1;
    SDL_SetError("Mixer IPC audio is not open");
    return 0;
}

static int begin_chunk(const MX_Message *request)
{
    Chunk_Record *record;
    if (!known_audio()) return -1;
    if (request->arg[0] <= 0 || request->arg[1] <= 0 ||
        (Uint32)request->arg[1] > 0x7fffffffUL / sizeof(Uint8) ||
        !native_int(request->arg[2], 0, MIX_MAX_VOLUME) ||
        find_chunk(request->arg[0])) {
        SDL_SetError("Invalid or duplicate mixer chunk upload");
        return -1;
    }
    record = (Chunk_Record *)SDL_calloc(1, sizeof(*record));
    if (!record) { SDL_OutOfMemory(); return -1; }
    record->data = (Uint8 *)SDL_malloc((Uint32)request->arg[1] * sizeof(Uint8));
    if (!record->data) { SDL_free(record); SDL_OutOfMemory(); return -1; }
    record->id = request->arg[0];
    record->length = (Uint32)request->arg[1];
    record->volume = (int)request->arg[2];
    record->next = chunks;
    chunks = record;
    return 0;
}

static int upload_data(const MX_Message *request)
{
    Chunk_Record *record = find_chunk(request->arg[0]);
    if (!record || record->chunk || request->arg[1] < 0 ||
        (Uint32)request->arg[1] != record->received || !request->length ||
        request->length > record->length - record->received) {
        SDL_SetError("Invalid or noncontiguous mixer chunk upload");
        return -1;
    }
    SDL_memcpy(record->data + record->received, request->payload,
               request->length * sizeof(Uint8));
    record->received += request->length;
    return 0;
}

static int commit_chunk(Sint32 id)
{
    Chunk_Record *record = find_chunk(id);
    if (!record || record->chunk || record->received != record->length) {
        SDL_SetError("Mixer chunk upload is missing or incomplete");
        return -1;
    }
    record->chunk = Mix_QuickLoad_RAW(record->data, record->length);
    if (!record->chunk) return -1;
    record->chunk->allocated = 1;
    Mix_VolumeChunk(record->chunk, record->volume);
    return 0;
}

static int drop_chunk(Sint32 id)
{
    Chunk_Record **link = &chunks, *record;
    while (*link && (*link)->id != id) link = &(*link)->next;
    if (!*link) { SDL_SetError("Unknown mixer chunk"); return -1; }
    record = *link;
    *link = record->next;
    free_record(record);
    return 0;
}

static Mix_Chunk *loaded_chunk(Sint32 id)
{
    Chunk_Record *record = find_chunk(id);
    if (!record || !record->chunk) {
        SDL_SetError("Unknown or incomplete mixer chunk");
        return NULL;
    }
    return record->chunk;
}

/* Only values with a known public int16 range are narrowed. Durations, sample
 * rates, identifiers and logical sample lengths remain explicitly 32-bit. */
static Sint32 dispatch(const MX_Message *request, MX_Message *reply)
{
    const Sint32 *arg = request->arg;
    Mix_Chunk *chunk;
    Chunk_Record *record;
    Sint32 rate, result = -1;
    Uint16 format;
    int channels, i;
    if (request->operation != MX_OPEN && request->operation != MX_QUIT &&
        !known_audio()) return -1;
    switch (request->operation) {
    case MX_OPEN:
        if (audio_open) { SDL_SetError("Mixer IPC audio is already open"); break; }
        if (!native_int(arg[1], 0, 65535L) ||
            !native_int(arg[2], 1, 2) || !native_int(arg[3], 1, 16384)) break;
        if (grow_completed(MIX_CHANNELS) < 0) break;
        result = Mix_OpenAudio(arg[0], (Uint16)arg[1], (int)arg[2], (int)arg[3]);
        if (result < 0) break;
        audio_open = 1;
        Mix_ChannelFinished(channel_finished);
        Mix_QuerySpec(&rate, &format, &channels);
        reply->arg[1] = rate;
        reply->arg[2] = format;
        reply->arg[3] = channels;
        break;
    case MX_ALLOCATE:
        if (!native_int(arg[0], -1, MIX_PCM_MAX_CHANNELS)) break;
        if (arg[0] > 0 && grow_completed((int)arg[0]) < 0) break;
        result = Mix_AllocateChannels((int)arg[0]);
        break;
    case MX_RESERVE:
        if (native_int(arg[0], -1, MIX_PCM_MAX_CHANNELS))
            result = Mix_ReserveChannels((int)arg[0]);
        break;
    case MX_BEGIN: result = begin_chunk(request); break;
    case MX_DATA: result = upload_data(request); break;
    case MX_COMMIT: result = commit_chunk(arg[0]); break;
    case MX_DROP: result = drop_chunk(arg[0]); break;
    case MX_PLAY:
        if (!channel_arg(arg[0], 1) ||
            !native_int(arg[2], -1, 32767) || arg[4] < -1) break;
        chunk = loaded_chunk(arg[1]);
        if (!chunk) break;
        result = arg[4] < 0 ?
            Mix_PlayChannelTimed((int)arg[0], chunk, (int)arg[2], arg[3]) :
            Mix_FadeInChannelTimed((int)arg[0], chunk, (int)arg[2], arg[4], arg[3]);
        break;
    case MX_HALT:
        if (channel_arg(arg[0], 1)) result = Mix_HaltChannel((int)arg[0]);
        break;
    case MX_EXPIRE:
        if (channel_arg(arg[0], 1)) result = Mix_ExpireChannel((int)arg[0], arg[1]);
        break;
    case MX_VOLUME:
        if (channel_arg(arg[0], 1) && native_int(arg[1], -1, 32767))
            result = Mix_Volume((int)arg[0], (int)arg[1]);
        break;
    case MX_CHUNK_VOLUME:
        if (!native_int(arg[1], -1, 32767)) break;
        chunk = loaded_chunk(arg[0]);
        if (chunk) result = Mix_VolumeChunk(chunk, (int)arg[1]);
        break;
    case MX_PAUSE:
        if (channel_arg(arg[0], 1)) { Mix_Pause((int)arg[0]); result = 0; }
        break;
    case MX_RESUME:
        if (channel_arg(arg[0], 1)) { Mix_Resume((int)arg[0]); result = 0; }
        break;
    case MX_PAUSED:
        if (channel_arg(arg[0], 1)) result = Mix_Paused((int)arg[0]);
        break;
    case MX_PLAYING:
        if (channel_arg(arg[0], 1)) result = Mix_Playing((int)arg[0]);
        break;
    case MX_FADEOUT:
        if (channel_arg(arg[0], 1)) result = Mix_FadeOutChannel((int)arg[0], arg[1]);
        break;
    case MX_FADING:
        if (channel_arg(arg[0], 0)) result = Mix_FadingChannel((int)arg[0]);
        break;
    case MX_GETCHUNK:
        if (!channel_arg(arg[0], 0)) break;
        chunk = Mix_GetChunk((int)arg[0]);
        result = 0;
        if (chunk) {
            for (record = chunks; record; record = record->next)
                if (record->chunk == chunk) { result = record->id; break; }
        }
        break;
    case MX_POLL:
        for (i = 0; i < completed_capacity && reply->length + 4 <= MX_PAYLOAD; ++i) {
            if (!completed[i]) continue;
            mx_put32(reply->payload + reply->length, (Uint32)i);
            reply->length += 4;
            completed[i] = 0;
        }
        result = 0;
        break;
    case MX_QUIT:
        cleanup();
        result = 0;
        break;
    default: SDL_SetError("Unknown mixer IPC operation"); break;
    }
    return result;
}

static void respond(const MX_Message *request, MX_Message *reply, Uint8 *wire)
{
    const char *error;
    Sint32 result;
    SDL_memset(reply, 0, sizeof(*reply));
    reply->request = request->request;
    reply->operation = request->operation | MX_REPLY;
    SDL_ClearError();
    result = dispatch(request, reply);
    reply->arg[0] = result;
    if (result < 0) {
        error = SDL_GetError();
        if (!error || !*error) error = "Invalid mixer IPC request";
        while (reply->length < MX_PAYLOAD && error[reply->length]) {
            reply->payload[reply->length] = (Uint8)error[reply->length];
            reply->length++;
        }
    }
    mx_encode(wire, reply);
    if (reply->length)
        SDL_memcpy(wire + MX_HEADER, reply->payload, reply->length * sizeof(Uint8));
}

static int descriptor(const char *text)
{
    Sint32 value = 0;
    if (!text || !*text) return -1;
    while (*text) {
        if (*text < '0' || *text > '9') return -1;
        value = value * 10 + (*text++ - '0');
        if (value >= FD_SETSIZE) return -1;
    }
    return (int)value;
}

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char **argv)
{
    MX_Message request, reply;
    Uint8 header[MX_HEADER], output[MX_HEADER + MX_PAYLOAD];
    int read_fd, write_fd, header_at = 0, payload_at = 0, complete = 0;
    int output_at = 0, output_size = 0, quitting = 0, result = 0;
    int changed, count, selected, maximum;
    Uint32 delay;
    fd_set reads, writes;
    struct timeval timeout;
    if (argc != 4 || SDL_strcmp(argv[1], "--ipc")) return 2;
    read_fd = descriptor(argv[2]); write_fd = descriptor(argv[3]);
    if (read_fd < 0 || write_fd < 0 || read_fd == write_fd ||
        nonblocking(read_fd) < 0 || nonblocking(write_fd) < 0) return 2;
    signal(SIGPIPE, SIG_IGN);
    if (SDL_Init(0) < 0) return 2;
    SDL_memset(&request, 0, sizeof(request));
    maximum = read_fd > write_fd ? read_fd + 1 : write_fd + 1;
    for (;;) {
        SDL_C16AudioPump();
        changed = 0;
        if (complete && !output_size && !quitting) {
            respond(&request, &reply, output);
            output_size = MX_HEADER + (int)reply.length;
            output_at = 0;
            quitting = request.operation == MX_QUIT;
            header_at = payload_at = complete = 0;
            changed = 1;
            SDL_C16AudioPump();
        }
        if (output_size) {
            count = write(write_fd, output + output_at, output_size - output_at);
            if (count > 0) {
                output_at += count;
                if (output_at == output_size) output_at = output_size = 0;
                changed = 1;
            } else if (!count || (errno != EINTR && errno != EAGAIN)) {
                result = 1; break;
            }
        }
        if (quitting && !output_size) break;
        /* At most one additional incoming frame is retained while its previous
         * reply is blocked. Reading it also observes ordinary parent EOF. */
        if (!complete && !quitting) {
            if (header_at < MX_HEADER) {
                count = read(read_fd, header + header_at, MX_HEADER - header_at);
                if (count > 0) {
                    header_at += count;
                    changed = 1;
                    if (header_at == MX_HEADER) {
                        if (mx_decode(&request, header) < 0 ||
                            (request.operation & MX_REPLY) ||
                            (request.length && request.operation != MX_DATA)) {
                            result = 1; break;
                        }
                        if (!request.length) complete = 1;
                    }
                } else if (!count) break;
                else if (errno != EINTR && errno != EAGAIN) { result = 1; break; }
            } else {
                count = read(read_fd, request.payload + payload_at,
                             (int)request.length - payload_at);
                if (count > 0) {
                    payload_at += count;
                    if ((Uint32)payload_at == request.length) complete = 1;
                    changed = 1;
                } else if (!count) break;
                else if (errno != EINTR && errno != EAGAIN) { result = 1; break; }
            }
        }
        if (changed) continue;
        FD_ZERO(&reads); FD_ZERO(&writes);
        if (!complete && !quitting) FD_SET(read_fd, &reads);
        if (output_size) FD_SET(write_fd, &writes);
        delay = SDL_C16AudioWait(1000UL);
        timeout.tv_sec = delay / 1000UL;
        timeout.tv_usec = (delay % 1000UL) * 1000UL;
        selected = select(maximum, &reads, &writes, NULL, &timeout);
        if (selected < 0 && errno != EINTR) { result = 1; break; }
    }
    cleanup();
    SDL_Quit();
    close(read_fd);
    close(write_fd);
    return result;
}
