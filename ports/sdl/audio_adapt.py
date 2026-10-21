#!/usr/bin/env python3
"""Generate SDL's bounded portable PCM conversion profile from pinned sources.

The filter bodies retained below are SDL 1.2.15's implementations. Packed
channel casts are replaced by audio_formats.c; unsupported multichannel/slow
resamplers are excluded. Both byte-addressed host and C16 use this profile.
"""
from pathlib import Path
import re

AUDIO_COMMON = ['src/audio/SDL_audiocvt.c']
FILTERS = ['SDL_Convert16LSB', 'SDL_Convert16MSB', 'SDL_Convert8',
           'SDL_ConvertSign', 'SDL_ConvertEndian', 'SDL_RateMUL2',
           'SDL_RateMUL2_c2', 'SDL_RateDIV2', 'SDL_RateDIV2_c2']


def _function(text, name):
    match = re.search(r'^void SDLCALL ' + re.escape(name) + r'\(', text, re.M)
    if not match:
        raise RuntimeError('Pinned SDL audio filter is missing: ' + name)
    start = match.start()
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    result = text[start:end].replace('int i;', 'Sint32 i;')
    # Upstream narrowing accidentally drops signedness before channel mixing.
    if name == 'SDL_Convert8':
        result = result.replace('~0x9010', '~0x1010')
    # Odd input frame counts must expose only complete frames actually written.
    if name in ('SDL_RateDIV2', 'SDL_RateDIV2_c2'):
        channels = 2 if name.endswith('_c2') else 1
        result = result.replace('cvt->len_cvt /= 2;',
            'cvt->len_cvt = (cvt->len_cvt / ((format & 255U) / 8 * ' +
            str(channels * 2) + ')) * ((format & 255U) / 8 * ' + str(channels) + ');')
    return result


def adapt_audio(source, c16=True):
    """Adapt a fresh pinned SDL source tree before its manifest is recorded."""
    source = Path(source)
    header = source / 'include/SDL_audio.h'
    text = header.read_text()
    replacements = [
        ('int freq;', 'Sint32 freq;'),
        ('int    len;', 'Sint32 len;'),
        ('int    len_cvt;', 'Sint32 len_cvt;'),
        ('Uint8 src_channels, int src_rate,', 'Uint8 src_channels, Sint32 src_rate,'),
        ('Uint8 dst_channels, int dst_rate);', 'Uint8 dst_channels, Sint32 dst_rate);'),
        ('} SDL_AudioCVT;',
         '\tUint8 src_channels, dst_channels; /* Profile frame-alignment metadata */\n} SDL_AudioCVT;'),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError('Pinned SDL audio header changed: ' + old)
        text = text.replace(old, new)
    text = text.replace('Currently raw and MS-ADPCM WAVE files are supported.',
                        'This profile supports mono/stereo 8/16-bit PCM WAVE files.')
    text = text.replace(" * to the other.\n", " * to the other. This profile accepts mono/stereo PCM and exact power-of-two\n"
                        " * sample-rate ratios; other ratios fail explicitly.\n")
    header.write_text(text)
    path = source / 'src/audio/SDL_audiocvt.c'
    upstream = path.read_text()
    preamble = upstream[:upstream.index('#include "SDL_config.h"')]
    generated = (preamble + '#include "SDL_config.h"\n#include "SDL_audio.h"\n'
                 '#include "audio_formats.h"\n\n'
                 'void SDLCALL SDL_ConvertMono(SDL_AudioCVT *, Uint16);\n'
                 'void SDLCALL SDL_ConvertStereo(SDL_AudioCVT *, Uint16);\n\n')
    generated += '\n\n'.join(_function(upstream, name) for name in FILTERS)
    generated += '\n\n' + CVT_PROFILE
    path.write_text(generated)


CVT_PROFILE = r'''/* Profile validation around SDL's portable octet filters. */
int SDL_ConvertAudio(SDL_AudioCVT *cvt)
{
    Uint32 frame;
    if (!cvt || !cvt->buf || cvt->len < 0 || cvt->len_mult < 1 ||
        !SDL_C16AudioFormatValid(cvt->src_format) ||
        !SDL_C16AudioFormatValid(cvt->dst_format) ||
        (cvt->src_channels != 1 && cvt->src_channels != 2) ||
        (cvt->dst_channels != 1 && cvt->dst_channels != 2)) {
        SDL_SetError("Invalid audio conversion buffer or description"); return -1;
    }
    frame = (cvt->src_format & 255U) / 8 * cvt->src_channels;
    if ((Uint32)cvt->len % frame ||
        (Uint32)cvt->len > 0x7fffffffUL / (Uint32)cvt->len_mult / sizeof(Uint8)) {
        SDL_SetError("Audio conversion length is unaligned or overflows"); return -1;
    }
    cvt->len_cvt = cvt->len;
    if (!cvt->len || !cvt->filters[0]) return 0;
    cvt->filter_index = 0;
    cvt->filters[0](cvt, cvt->src_format);
    return 0;
}

int SDL_BuildAudioCVT(SDL_AudioCVT *cvt,
    Uint16 src_format, Uint8 src_channels, Sint32 src_rate,
    Uint16 dst_format, Uint8 dst_channels, Sint32 dst_rate)
{
    Uint32 high, low;
    int rates = 0, n = 0;
    void (SDLCALL *rate_filter)(SDL_AudioCVT *, Uint16);
    if (!cvt) { SDL_SetError("Missing audio conversion description"); return -1; }
    SDL_memset(cvt, 0, sizeof(*cvt));
    if (!SDL_C16AudioFormatValid(src_format) || !SDL_C16AudioFormatValid(dst_format) ||
        (src_channels != 1 && src_channels != 2) ||
        (dst_channels != 1 && dst_channels != 2) ||
        src_rate < 1000 || src_rate > 192000L ||
        dst_rate < 1000 || dst_rate > 192000L) {
        SDL_SetError("Unsupported PCM format, channels or sample rate"); return -1;
    }
    high = src_rate > dst_rate ? src_rate : dst_rate;
    low = src_rate > dst_rate ? dst_rate : src_rate;
    while (low < high && low <= high / 2) { low *= 2; ++rates; }
    if (low != high) {
        SDL_SetError("Only exact power-of-two audio rate ratios are supported"); return -1;
    }
    cvt->src_format = src_format; cvt->dst_format = dst_format;
    cvt->src_channels = src_channels; cvt->dst_channels = dst_channels;
    cvt->len_mult = 1; cvt->len_ratio = 1.0;
    if ((src_format & 0x1000U) != (dst_format & 0x1000U) &&
        (src_format & 255U) == 16 && (dst_format & 255U) == 16)
        cvt->filters[n++] = SDL_ConvertEndian;
    if ((src_format & 0x8000U) != (dst_format & 0x8000U))
        cvt->filters[n++] = SDL_ConvertSign;
    if ((src_format & 255U) != (dst_format & 255U)) {
        if ((dst_format & 255U) == 8) {
            cvt->filters[n++] = SDL_Convert8; cvt->len_ratio /= 2;
        } else {
            cvt->filters[n++] = (dst_format & 0x1000U) ? SDL_Convert16MSB : SDL_Convert16LSB;
            cvt->len_mult *= 2; cvt->len_ratio *= 2;
        }
    }
    if (src_channels != dst_channels) {
        if (dst_channels == 2) {
            cvt->filters[n++] = SDL_ConvertStereo;
            cvt->len_mult *= 2; cvt->len_ratio *= 2;
        } else {
            cvt->filters[n++] = SDL_ConvertMono; cvt->len_ratio /= 2;
        }
    }
    if (n + rates >= 10) {
        SDL_memset(cvt, 0, sizeof(*cvt));
        SDL_SetError("Audio conversion needs too many filters"); return -1;
    }
    if (src_rate < dst_rate)
        rate_filter = dst_channels == 2 ? SDL_RateMUL2_c2 : SDL_RateMUL2;
    else
        rate_filter = dst_channels == 2 ? SDL_RateDIV2_c2 : SDL_RateDIV2;
    while (rates--) {
        cvt->filters[n++] = rate_filter;
        if (src_rate < dst_rate) { cvt->len_mult *= 2; cvt->len_ratio *= 2; }
        else cvt->len_ratio /= 2;
    }
    cvt->filters[n] = NULL;
    cvt->needed = n != 0;
    return cvt->needed;
}
'''
