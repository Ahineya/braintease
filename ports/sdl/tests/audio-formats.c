/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Portable audio fixtures. Host wide carriers model C16 logical octets. */
#include "SDL_audio.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#ifndef PROFILE_REFERENCE
#include "audio_formats.h"
#endif
static Uint16 mixing_format;
Uint16 SDL_C16AudioMixFormat(void) { return mixing_format; }
#ifdef PROFILE_REFERENCE
struct ref_audio { SDL_AudioCVT convert; SDL_AudioSpec spec; } ref_device;
struct ref_audio *current_audio = &ref_device;
#endif
static char error_text[256];
static int closed;
void SDL_SetError(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(error_text, sizeof(error_text), fmt, ap); va_end(ap);
}
void SDL_Error(SDL_errorcode code) { SDL_SetError("SDL error %d", (int)code); }
char *SDL_GetError(void) { return error_text; }
void SDL_ClearError(void) { error_text[0] = 0; }
static int fail(const char *what) { fprintf(stderr, "%s: %s\n", what, error_text); return 1; }
static Uint32 hash(const Uint8 *p, Uint32 n)
{
    Uint32 h = 2166136261UL, i;
    for (i = 0; i < n; ++i) { if (p[i] > 255U) abort(); h = (h ^ p[i]) * 16777619UL; }
    return h;
}
static Sint32 mem_read(SDL_RWops *r, void *dst, Sint32 size, Sint32 count)
{
    Uint32 i, n = (Uint32)size * count, avail = r->hidden.mem.stop - r->hidden.mem.here;
    if (size <= 0 || count <= 0) return 0;
    if (n > avail) n = avail - avail % size;
    for (i = 0; i < n; ++i) ((Uint8 *)dst)[i] = r->hidden.mem.here[i];
    r->hidden.mem.here += n; return n / size;
}
static Sint32 mem_seek(SDL_RWops *r, Sint32 off, Sint32 whence)
{
    Sint32 pos = whence == RW_SEEK_SET ? 0 :
              whence == RW_SEEK_END ? r->hidden.mem.stop-r->hidden.mem.base : r->hidden.mem.here-r->hidden.mem.base;
    pos += off;
    if (pos < 0) pos = 0;
    if (pos > r->hidden.mem.stop-r->hidden.mem.base) pos = r->hidden.mem.stop-r->hidden.mem.base;
    r->hidden.mem.here = r->hidden.mem.base + pos; return pos;
}
static Sint32 mem_close(SDL_RWops *r) { (void)r; ++closed; return 0; }
static void stream(SDL_RWops *r, Uint8 *p, Sint32 n)
{
    memset(r, 0, sizeof(*r)); r->read = mem_read; r->seek = mem_seek; r->close = mem_close;
    r->hidden.mem.base = r->hidden.mem.here = p; r->hidden.mem.stop = p+n;
}
/* Upstream WAV loader's endian stream reads for the byte reference. */
Uint32 SDL_ReadLE32(SDL_RWops *r)
{
    Uint8 p[4]; if (SDL_RWread(r,p,1,4) != 4) return 0;
    return p[0] | ((Uint32)p[1]<<8) | ((Uint32)p[2]<<16) | ((Uint32)p[3]<<24);
}
static void u16(Uint8 *p, unsigned v) { p[0]=v&255; p[1]=(v>>8)&255; }
static void u32(Uint8 *p, Uint32 v) { u16(p,v&65535U); u16(p+2,v>>16); }
static void four(Uint8 *p, const char *s) { int i; for(i=0;i<4;i++)p[i]=s[i]; }
static int make_wave(Uint8 *p, int channels, int bits, int ancillary)
{
    int at = 12, i, count = 32*channels*(bits/8);
    four(p,"RIFF");four(p+8,"WAVE");
    if(ancillary) { four(p+at,"JUNK");u32(p+at+4,3);p[at+8]=9;p[at+9]=8;p[at+10]=7;p[at+11]=0;at+=12; }
    four(p+at,"fmt ");u32(p+at+4,16);u16(p+at+8,1);u16(p+at+10,channels);
    u32(p+at+12,44100);u32(p+at+16,44100UL*channels*(bits/8));u16(p+at+20,channels*(bits/8));u16(p+at+22,bits);at+=24;
    four(p+at,"data");u32(p+at+4,count);at+=8;
    for(i=0;i<count;i++)p[at+i]=(i*53+19)^((i>>1)&255);
    for(i=0;i<count;i++)p[at+i]&=255;
    at+=count;u32(p+4,at-8);return at;
}
int main(void)
{
    Uint16 formats[] = {AUDIO_U8,AUDIO_S8,AUDIO_U16LSB,AUDIO_S16LSB,AUDIO_U16MSB,AUDIO_S16MSB};
    int a,b,sc,dc,rr,i,volume;
    Uint8 storage[4096], original[4096], wave[1024];
    SDL_AudioCVT cvt;
    SDL_RWops rw;
    SDL_AudioSpec spec;
    Uint8 *loaded;
    Uint32 loaded_len;
    int count;
    for(a=0;a<6;a++)for(b=0;b<6;b++)for(sc=1;sc<=2;sc++)for(dc=1;dc<=2;dc++)for(rr=0;rr<3;rr++) {
        Sint32 src_rate=rr==2?44100L:22050L, dst_rate=rr==1?44100L:22050L;
        int len=32*sc*((formats[a]&255)/8), cap;
        if(SDL_BuildAudioCVT(&cvt,formats[a],sc,src_rate,formats[b],dc,dst_rate)<0)return fail("build conversion");
        cap=len*cvt.len_mult;
        for(i=0;i<4096;i++)storage[i]=0xa5;
        for(i=0;i<len;i++)storage[8+i]=((i*53+19)^(i>>1))&255;
        cvt.buf=storage+8;cvt.len=len;
        if(SDL_ConvertAudio(&cvt)<0)return fail("convert");
        for(i=0;i<8;i++)if(storage[i]!=0xa5||storage[8+cap+i]!=0xa5)return fail("conversion guard");
        printf("CVT %d %d %d %d %d %ld %lu\n",a,b,sc,dc,rr,(long)cvt.len_cvt,(unsigned long)hash(cvt.buf,cvt.len_cvt));
    }
    for(a=0;a<6;a++)for(volume=0;volume<=128;volume+=32) {
#ifdef PROFILE_REFERENCE
        if(a==2||a==4)continue; /* Upstream mixer lacks unsigned 16-bit support. */
        ref_device.spec.format=formats[a];
#else
        mixing_format=formats[a];
#endif
        for(i=0;i<259;i++){storage[i]=((i*31+129)^(i>>1))&255;original[i]=((i*53+71)^(i>>1))&255;}
        SDL_MixAudio(storage+1,original+1,257,volume);
        if(storage[0]!=129 || storage[258]!=(((258*31+129)^(258>>1))&255))return fail("mixer guard");
        printf("MIX %d %d %lu\n",a,volume,(unsigned long)hash(storage,259));
    }
    for(sc=1;sc<=2;sc++)for(a=0;a<2;a++) {
        count=make_wave(wave,sc,a?16:8,0);stream(&rw,wave,count);closed=0;
        if(!SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1)return fail("valid WAV");
        printf("WAV %d %d %ld %u %u %lu %lu\n",sc,a,(long)spec.freq,(unsigned)spec.format,(unsigned)spec.channels,(unsigned long)loaded_len,(unsigned long)hash(loaded,loaded_len));
        SDL_FreeWAV(loaded);
    }
#ifndef PROFILE_REFERENCE
    /* Every prefix of a valid file must fail, clear outputs, and close once. */
    count=make_wave(wave,2,16,1);
    for(i=0;i<count;i++) {
        stream(&rw,wave,i);closed=0;loaded=(Uint8 *)1;loaded_len=1234;
        if(SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1||loaded||loaded_len)return fail("truncated WAV");
    }
    stream(&rw,wave,count);closed=0;
    if(!SDL_LoadWAV_RW(&rw,0,&spec,&loaded,&loaded_len)||closed||SDL_RWtell(&rw)!=count)return fail("odd ancillary WAV");
    SDL_FreeWAV(loaded);
    /* Legacy effects can omit the final odd PCM data pad from RIFF length.
     * Accept a complete payload, but keep rejecting every truncated prefix. */
    {
        int legacy_length;
        count=make_wave(wave,1,8,0);
        legacy_length=count-1;
        u32(wave+4,legacy_length-8);u32(wave+40,31);
        stream(&rw,wave,legacy_length);closed=0;
        if(!SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1||loaded_len!=31)
            return fail("legacy terminal odd PCM data");
        SDL_FreeWAV(loaded);
        stream(&rw,wave,legacy_length-1);closed=0;loaded=(Uint8 *)1;loaded_len=1;
        if(SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1||loaded||loaded_len)
            return fail("truncated legacy terminal PCM data");
    }
    {
        Uint32 j;
        Uint8 *large = (Uint8 *)malloc(40046UL * sizeof(Uint8));
        if(!large)return fail("large WAV allocation");
        make_wave(large,2,8,0);u32(large+4,40038UL);u32(large+40,40001UL);
        for(j=0;j<40002UL;j++)large[44+j]=(j*7+3)&255;
        stream(&rw,large,40046);closed=0;
        if(!SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1||loaded_len!=40000UL||
           spec.freq!=44100L||hash(loaded,loaded_len)!=hash(large+44,40000UL))return fail("large odd PCM WAV");
        SDL_FreeWAV(loaded);free(large);
    }
    /* Broken length, encoding, channels, rate, byte rate, alignment, depth. */
    for(a=0;a<9;a++) {
        count=make_wave(wave,2,16,0);
        switch(a){case 0:u32(wave+4,0xffffffffUL);break;case 1:u32(wave+16,15);break;case 2:u16(wave+20,17);break;case 3:u16(wave+22,0);break;case 4:u32(wave+24,0);break;case 5:u32(wave+28,1);break;case 6:u16(wave+32,3);break;case 7:u16(wave+34,24);break;case 8:u32(wave+40,0xffffffffUL);break;}
        stream(&rw,wave,count);closed=0;loaded=(Uint8 *)1;loaded_len=1;
        if(SDL_LoadWAV_RW(&rw,1,&spec,&loaded,&loaded_len)||closed!=1||loaded||loaded_len)return fail("malformed WAV");
    }
    if(SDL_BuildAudioCVT(&cvt,AUDIO_U8,1,44100,AUDIO_U8,1,48000)!=-1)return fail("nonbinary rate");
    if(SDL_BuildAudioCVT(&cvt,AUDIO_U8,0,22050,AUDIO_U8,1,22050)!=-1)return fail("bad channels");
    if(SDL_BuildAudioCVT(&cvt,0x18,1,22050,AUDIO_U8,1,22050)!=-1)return fail("bad format");
    if(SDL_BuildAudioCVT(&cvt,AUDIO_S16LSB,2,44100,AUDIO_S16LSB,2,22050)<0)return fail("odd downsample build");
    cvt.buf=storage;cvt.len=12;
    if(SDL_ConvertAudio(&cvt)||cvt.len_cvt!=4)return fail("partial downsample frame");
    cvt.len=3;if(SDL_ConvertAudio(&cvt)!=-1)return fail("unaligned conversion");
    cvt.len=0x7fffffffL;if(SDL_ConvertAudio(&cvt)!=-1)return fail("overflow conversion");
    if(SDL_BuildAudioCVT(&cvt,AUDIO_U8,1,22050,AUDIO_S16LSB,2,44100)<0)return fail("overflow build");
    cvt.buf=storage;cvt.len=0x40000000L;
    if(SDL_ConvertAudio(&cvt)!=-1)return fail("expansion overflow");
    mixing_format=0;storage[0]=91;SDL_MixAudio(storage,original,1,128);if(storage[0]!=91)return fail("closed mixer");
    printf("CHECKS %d\n",count+16);
#endif
    return 0;
}
