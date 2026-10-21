/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable adaptation of SDL 1.2 PCM mixing, channel conversion and WAV IO.
 * SDL - Simple DirectMedia Layer, Copyright (C) 1997-2012 Sam Lantinga.
 * Retains SDL's sample quantization, integer averaging and saturating mix.
 * All serialized offsets and lengths count logical octets, never scalar layout.
 */
#include "SDL_audio.h"
#include "audio_formats.h"

int SDL_C16AudioFormatValid(Uint16 format)
{
    return format == AUDIO_U8 || format == AUDIO_S8 ||
           format == AUDIO_U16LSB || format == AUDIO_S16LSB ||
           format == AUDIO_U16MSB || format == AUDIO_S16MSB;
}

/* Unsigned formats stay unsigned here. The converter averages unsigned PCM
 * before any rebias, as the upstream filter does. Signed octets are decoded
 * explicitly: casting a logical Uint8 carrier to Sint8 is incorrect on C16. */
Sint32 SDL_C16AudioReadSample(const Uint8 *src, Uint16 format)
{
    Sint32 value;
    if ((format & 255U) == 8) {
        value = src[0] & 255U;
        if ((format & 0x8000U) && value >= 128) value -= 256;
    } else {
        if (format & 0x1000U)
            value = ((Uint32)(src[0] & 255U) << 8) | (src[1] & 255U);
        else
            value = ((Uint32)(src[1] & 255U) << 8) | (src[0] & 255U);
        if ((format & 0x8000U) && value >= 32768L) value -= 65536L;
    }
    return value;
}

void SDL_C16AudioWriteSample(Uint8 *dst, Uint16 format, Sint32 sample)
{
    Uint32 value = (Uint32)sample;
    if ((format & 255U) == 8) {
        dst[0] = value & 255U;
    } else if (format & 0x1000U) {
        dst[0] = (value >> 8) & 255U; dst[1] = value & 255U;
    } else {
        dst[0] = value & 255U; dst[1] = (value >> 8) & 255U;
    }
}

void SDLCALL SDL_ConvertMono(SDL_AudioCVT *cvt, Uint16 format)
{
    Sint32 i, sample;
    int bytes = (format & 255U) / 8;
    Uint8 *src = cvt->buf, *dst = cvt->buf;
    for (i = cvt->len_cvt / (bytes * 2); i; --i) {
        sample = SDL_C16AudioReadSample(src, format) +
                 SDL_C16AudioReadSample(src + bytes, format);
        SDL_C16AudioWriteSample(dst, format, sample / 2);
        src += bytes * 2; dst += bytes;
    }
    cvt->len_cvt /= 2;
    if (cvt->filters[++cvt->filter_index])
        cvt->filters[cvt->filter_index](cvt, format);
}

void SDLCALL SDL_ConvertStereo(SDL_AudioCVT *cvt, Uint16 format)
{
    Sint32 i;
    int bytes = (format & 255U) / 8;
    Uint8 *src = cvt->buf + cvt->len_cvt;
    Uint8 *dst = cvt->buf + cvt->len_cvt * 2;
    for (i = cvt->len_cvt / bytes; i; --i) {
        src -= bytes; dst -= bytes * 2;
        if (bytes == 2) {
            /* Read both before writing: source and destination can coincide. */
            Uint8 lo = src[0], hi = src[1];
            dst[0] = lo; dst[1] = hi; dst[2] = lo; dst[3] = hi;
        } else {
            dst[0] = src[0]; dst[1] = src[0];
        }
    }
    cvt->len_cvt *= 2;
    if (cvt->filters[++cvt->filter_index])
        cvt->filters[cvt->filter_index](cvt, format);
}

void SDL_C16MixAudioFormat(Uint8 *dst, const Uint8 *src,
                          Uint32 len, Uint16 format, int volume)
{
    Sint32 a, b, result, minimum, maximum;
    int bytes;
    if (!len || volume == 0) return;
    if (!dst || !src || volume < 0 || volume > SDL_MIX_MAXVOLUME ||
        !SDL_C16AudioFormatValid(format)) {
        SDL_SetError("Invalid PCM mix parameters"); return;
    }
    bytes = (format & 255U) / 8;
    minimum = bytes == 1 ? -128 : -32768L;
    maximum = bytes == 1 ? 127 : 32767L;
    while (len >= (Uint32)bytes) {
        a = SDL_C16AudioReadSample(src, format);
        b = SDL_C16AudioReadSample(dst, format);
        if (!(format & 0x8000U)) {
            a += minimum; b += minimum;
        }
        result = b + (a * (Sint32)volume) / SDL_MIX_MAXVOLUME;
        if (result < minimum) result = minimum;
        if (result > maximum) result = maximum;
        if (!(format & 0x8000U)) {
            result -= minimum;
            /* The legacy U8 mix8 table deliberately clips to 254. */
            if (bytes == 1 && result > 254) result = 254;
        }
        SDL_C16AudioWriteSample(dst, format, result);
        src += bytes; dst += bytes; len -= bytes;
    }
}

void SDL_MixAudio(Uint8 *dst, const Uint8 *src, Uint32 len, int volume)
{
    Uint16 format = SDL_C16AudioMixFormat();
    if (!format) { SDL_SetError("Audio device is not open"); return; }
    SDL_C16MixAudioFormat(dst, src, len, format, volume);
}

static Uint16 wav_u16(const Uint8 *p)
{
    return (p[0] & 255U) | ((Uint16)(p[1] & 255U) << 8);
}
static Uint32 wav_u32(const Uint8 *p)
{
    return (Uint32)(p[0] & 255U) | ((Uint32)(p[1] & 255U) << 8) |
           ((Uint32)(p[2] & 255U) << 16) | ((Uint32)(p[3] & 255U) << 24);
}
static int wav_id(const Uint8 *p, const char *id)
{
    return p[0] == id[0] && p[1] == id[1] && p[2] == id[2] && p[3] == id[3];
}
static int wav_read(SDL_RWops *src, Uint8 *p, Uint32 count)
{
    return count <= 0x7fffffffUL && SDL_RWread(src, p, 1, (Sint32)count) == (Sint32)count;
}
/* Discard by reading, so clamping memory-stream seeks cannot hide truncation. */
static int wav_skip(SDL_RWops *src, Uint32 count)
{
    Uint8 scratch[256];
    Uint32 take;
    while (count) {
        take = count > 256 ? 256 : count;
        if (!wav_read(src, scratch, take)) return 0;
        count -= take;
    }
    return 1;
}

SDL_AudioSpec *SDL_LoadWAV_RW(SDL_RWops *src, int freesrc,
                             SDL_AudioSpec *spec, Uint8 **audio_buf,
                             Uint32 *audio_len)
{
    Uint8 header[16], *data = NULL;
    Uint32 remaining, length, padded, rate = 0, data_length = 0;
    Uint16 channels = 0, bits = 0, align = 0;
    SDL_AudioSpec result;
    int have_format = 0, ok = 0;
    if (audio_buf) *audio_buf = NULL;
    if (audio_len) *audio_len = 0;
    if (!src || !spec || !audio_buf || !audio_len) {
        SDL_SetError("Invalid WAV arguments"); goto done;
    }
    if (!wav_read(src, header, 12) || !wav_id(header, "RIFF") ||
        !wav_id(header + 8, "WAVE")) {
        SDL_SetError("Unrecognized or truncated WAVE header"); goto done;
    }
    remaining = wav_u32(header + 4);
    if (remaining < 4 || remaining > 0x7fffffffUL) {
        SDL_SetError("Invalid WAVE container length"); goto done;
    }
    remaining -= 4;
    while (remaining) {
        if (remaining < 8 || !wav_read(src, header, 8)) {
            SDL_SetError("Truncated WAVE chunk header"); goto done;
        }
        remaining -= 8;
        length = wav_u32(header + 4);
        /* Check before addition, including the RIFF odd-octet pad. */
        if (length > remaining) {
            SDL_SetError("WAVE chunk exceeds its container"); goto done;
        }
        padded = length + (length & 1U);
        /* Old PCM writers sometimes exclude the terminal data pad from RIFF
         * size (or omit it entirely). Accept only that final data exception;
         * a declared pad or an odd ancillary chunk must still be complete. */
        if ((length & 1U) && length == remaining) {
            if (!wav_id(header, "data") || !have_format) {
                SDL_SetError("WAVE chunk padding exceeds its container"); goto done;
            }
            padded = length;
        }
        if (wav_id(header, "fmt ")) {
            if (have_format || length < 16 || !wav_read(src, header, 16)) {
                SDL_SetError("Invalid WAVE format chunk"); goto done;
            }
            channels = wav_u16(header + 2); rate = wav_u32(header + 4);
            align = wav_u16(header + 12); bits = wav_u16(header + 14);
            if (wav_u16(header) != 1 || (channels != 1 && channels != 2) ||
                (bits != 8 && bits != 16) || rate < 1000 || rate > 192000UL ||
                align != channels * (bits / 8) ||
                wav_u32(header + 8) != rate * (Uint32)align) {
                SDL_SetError("Unsupported or inconsistent PCM WAVE format"); goto done;
            }
            have_format = 1;
            if (!wav_skip(src, padded - 16)) {
                SDL_SetError("Truncated WAVE format data"); goto done;
            }
        } else if (wav_id(header, "data") && !data) {
            if (!have_format || length > 0x7fffffffUL / sizeof(Uint8)) {
                SDL_SetError("Missing WAVE format or oversized audio data"); goto done;
            }
            /* Keep any partial final frame allocated, but expose full frames. */
            data = (Uint8 *)SDL_malloc((length ? length : 1) * sizeof(Uint8));
            if (!data) { SDL_Error(SDL_ENOMEM); goto done; }
            if (!wav_read(src, data, length) || !wav_skip(src, padded - length)) {
                SDL_SetError("Truncated WAVE audio data"); goto done;
            }
            data_length = length - length % align;
        } else if (!wav_skip(src, padded)) {
            SDL_SetError("Truncated WAVE ancillary data"); goto done;
        }
        remaining -= padded;
    }
    if (!have_format || !data) {
        SDL_SetError("WAVE is missing PCM format or audio data"); goto done;
    }
    SDL_memset(&result, 0, sizeof(result));
    result.freq = rate; result.channels = channels;
    result.format = bits == 8 ? AUDIO_U8 : AUDIO_S16LSB;
    result.samples = 4096;
    *spec = result; *audio_buf = data; *audio_len = data_length;
    data = NULL; ok = 1;
done:
    if (data) SDL_free(data);
    if (src && freesrc) SDL_RWclose(src);
    return ok ? spec : NULL;
}

void SDL_FreeWAV(Uint8 *audio_buf) { SDL_free(audio_buf); }
