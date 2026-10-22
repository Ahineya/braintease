/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Shared local WAV conversion; no process, callback or device ownership. */
static Uint32 load_fraction(Uint32 part, Uint32 whole, Uint32 scale)
{
    Uint32 bit = 65536UL, result = 0, remainder = 0;
    if (!whole || part >= whole) return scale;
    while (bit > scale && bit > 1) bit >>= 1;
    for (; bit; bit >>= 1) {
        result *= 2;
        if (remainder >= whole - remainder) {
            remainder -= whole - remainder;
            result++;
        } else remainder *= 2;
        if (scale & bit) {
            if (remainder >= whole - part) {
                remainder -= whole - part;
                result++;
            } else remainder += part;
        }
    }
    return result;
}

/* Resample only while loading, using floor(source_frame * dst/src) length
 * and nearest source frame floor(output_frame * src/dst). No scalar casts. */
static Uint8 *resample(const Uint8 *source, Uint32 source_frames, Uint32 frame_bytes,
                       Uint32 src_rate, Uint32 dst_rate, Uint32 *length)
{
    Uint32 whole, frames, tail, frame, index = 0, phase = 0, step, remainder;
    Uint8 *output;
    whole = source_frames / src_rate;
    tail = load_fraction(source_frames % src_rate, src_rate, dst_rate);
    if (whole > (0x7fffffffUL - tail) / dst_rate) goto overflow;
    frames = whole * dst_rate + tail;
    if (!frames) { SDL_SetError("WAVE is shorter than one output frame"); return NULL; }
    if (frames > 0x7fffffffUL / frame_bytes / sizeof(Uint8)) goto overflow;
    *length = frames * frame_bytes;
    output = (Uint8 *)SDL_malloc(*length * sizeof(Uint8));
    if (!output) { SDL_OutOfMemory(); return NULL; }
    step = src_rate / dst_rate; remainder = src_rate % dst_rate;
    for (frame = 0; frame < frames; ++frame) {
        SDL_memcpy(output + frame * frame_bytes, source + index * frame_bytes,
                   frame_bytes * sizeof(Uint8));
        index += step;
        phase += remainder;
        if (phase >= dst_rate) { phase -= dst_rate; index++; }
    }
    return output;
overflow:
    SDL_SetError("Resampled WAVE exceeds PCM storage bounds"); return NULL;
}

static Mix_Chunk *mix_load_wav(SDL_RWops *src, int freesrc, SDL_AudioSpec target)
{
    SDL_AudioSpec input;
    SDL_AudioCVT cvt;
    Uint8 *data = NULL, *converted, *output;
    Uint32 length, capacity, frame_bytes;
    Mix_Chunk *chunk;
    if (!SDL_LoadWAV_RW(src, freesrc, &input, &data, &length)) return NULL;
    if (!length) { SDL_SetError("WAVE contains no complete audio frames"); goto fail; }
    if (SDL_BuildAudioCVT(&cvt, input.format, input.channels, input.freq,
                          target.format, target.channels, input.freq) < 0) goto fail;
    if (length > 0x7fffffffUL / (Uint32)cvt.len_mult / sizeof(Uint8)) {
        SDL_SetError("Converted WAVE exceeds PCM storage bounds"); goto fail;
    }
    if (cvt.needed) {
        capacity = length * (Uint32)cvt.len_mult;
        converted = (Uint8 *)SDL_malloc(capacity * sizeof(Uint8));
        if (!converted) { SDL_OutOfMemory(); goto fail; }
        SDL_memcpy(converted, data, length * sizeof(Uint8));
        SDL_FreeWAV(data); data = converted;
        cvt.buf = data; cvt.len = (Sint32)length;
        if (SDL_ConvertAudio(&cvt) < 0) goto fail;
        length = (Uint32)cvt.len_cvt;
    }
    frame_bytes = (target.format & 255U) / 8 * target.channels;
    if (input.freq != target.freq) {
        output = resample(data, length / frame_bytes, frame_bytes,
                          (Uint32)input.freq, (Uint32)target.freq, &length);
        if (!output) goto fail;
        SDL_FreeWAV(data); data = output;
    }
    chunk = (Mix_Chunk *)SDL_malloc(sizeof(*chunk));
    if (!chunk) { SDL_OutOfMemory(); goto fail; }
    chunk->allocated = 1; chunk->abuf = data; chunk->alen = length; chunk->volume = 128;
    return chunk;
fail:
    SDL_FreeWAV(data); return NULL;
}

