#!/usr/bin/env python3
"""Prepare a verified SDL release and a separate C16 software profile."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import urllib.request
try:
    from .audio_adapt import adapt_audio, AUDIO_COMMON
except ImportError:
    from audio_adapt import adapt_audio, AUDIO_COMMON

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
COMMON = ['src/SDL.c', 'src/SDL_error.c',
          'src/events/SDL_active.c', 'src/events/SDL_events.c',
          'src/events/SDL_expose.c', 'src/events/SDL_keyboard.c',
          'src/events/SDL_mouse.c', 'src/events/SDL_quit.c',
          'src/events/SDL_resize.c', 'src/file/SDL_rwops.c',
          'src/video/SDL_video.c', 'src/video/SDL_surface.c',
          'src/video/SDL_pixels.c', 'src/video/SDL_blit.c',
          'src/video/SDL_bmp.c', 'src/video/SDL_cursor.c',
          'src/stdlib/SDL_string.c', *AUDIO_COMMON]


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('SDL source adaptation changed: ' + old[:100])
    return text.replace(old, new)


def prepare(build, fetch=False, c16=True):
    build = Path(build)
    pin = json.loads((PORT / 'PROVENANCE.json').read_text())
    archive = ROOT / 'build/downloads/SDL-1.2.15.tar.gz'
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        if not fetch:
            raise RuntimeError('Missing pinned SDL archive; use --fetch')
        temporary = archive.with_suffix('.download')
        with urllib.request.urlopen(pin['url'], timeout=60) as response:
            temporary.write_bytes(response.read())
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != pin['sha256']:
            temporary.unlink()
            raise RuntimeError('SDL archive checksum mismatch')
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        raise RuntimeError('SDL archive checksum mismatch')
    pristine = build / 'pristine'
    pristine.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        tar.extractall(pristine, filter='data')
    original = pristine / 'SDL-1.2.15'
    source = build / ('c16' if c16 else 'host')
    shutil.copytree(original, source, dirs_exist_ok=True)
    # The release has a Latin-1 author credit in default_cursor.h.
    for path in [*source.rglob('*.c'), *source.rglob('*.h')]:
        try:
            path.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            path.write_text(path.read_bytes().decode('latin-1'), encoding='utf-8')
    license_dir = build / 'licenses'
    license_dir.mkdir(exist_ok=True)
    shutil.copy2(original / 'COPYING', license_dir / 'SDL-COPYING.txt')
    (source / 'include/SDL_config.h').write_text(config(c16))
    if c16:
        adapt_c16(source)
    adapt_audio(source, c16)
    # Callbacks share the app's memory and run only at explicit SDL boundaries.
    # Timer/delay waits are capped by audio refill deadlines. This is a bounded
    # cooperative profile, not a substitute for a guest thread implementation.
    path = source / 'src/events/SDL_events.c'
    text = path.read_text()
    text = text.replace('#include "SDL_events_c.h"', '#include "SDL_events_c.h"\n#include "audio.h"')
    text = replace(text, 'void SDL_PumpEvents(void)\n{',
                   'void SDL_PumpEvents(void)\n{\n SDL_C16AudioPump();')
    if c16:
        text = replace(text, 'SDL_GX_Wait(SDL_C16RepeatWait())',
                       'SDL_GX_Wait(SDL_C16AudioWait(SDL_C16RepeatWait()))')
    path.write_text(text)
    # This backend is deliberately separate from rendering representation.
    path = source / 'src/video/SDL_video.c'
    text = path.read_text()
    text = replace(text, 'static VideoBootStrap *bootstrap[] = {',
        'extern VideoBootStrap GX_bootstrap;\nstatic VideoBootStrap *bootstrap[] = {\n#if SDL_VIDEO_DRIVER_GX\n &GX_bootstrap,\n#endif')
    path.write_text(text)
    path = source / 'src/video/SDL_video.c'
    text = path.read_text().replace('#include "SDL_sysvideo.h"',
                                    '#include "SDL_sysvideo.h"\n#include "audio.h"')
    for name in ('SDL_Flip', 'SDL_UpdateRects'):
        pattern = r'((?:int|void) ' + name + r'\s*\([^\n]*\)\n\{)'
        text, count = re.subn(pattern, r'\1\n SDL_C16AudioPump();', text)
        if count != 1:
            raise RuntimeError('SDL audio presentation boundary changed: ' + name)
    path.write_text(text)
    # SDL buttons are numbered 1..8; zero makes SDL_BUTTON shift by -1.
    # The logical Uint8 carrier is wider than an octet on C16, so sizeof
    # must not determine the protocol's button count.
    path = source / 'src/events/SDL_mouse.c'
    text = path.read_text()
    text = replace(text, 'for ( i = 0; i < sizeof(SDL_ButtonState)*8; ++i )',
                   'for ( i = 1; i <= 8; ++i )')
    path.write_text(text)
    changed = {}
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        before = original / relative
        if path.is_file() and (not before.exists() or path.read_bytes() != before.read_bytes()):
            changed[str(relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    (build / ('adaptation-c16.json' if c16 else 'adaptation-host.json')).write_text(
        json.dumps(dict(upstream=pin, profile='C16 logical octets' if c16 else 'host octets',
                        files=changed), indent=2) + '\n')
    return source


def config(c16):
    names = ['STDC_HEADERS', 'HAVE_LIBC', 'HAVE_STDIO_H', 'HAVE_SYS_TYPES_H',
             'HAVE_STDINT_H', 'HAVE_STRING_H', 'HAVE_STRINGS_H', 'HAVE_CTYPE_H',
             'SDL_HAS_64BIT_TYPE', 'HAVE_MALLOC', 'HAVE_CALLOC', 'HAVE_REALLOC',
             'HAVE_FREE', 'HAVE_GETENV', 'HAVE_PUTENV', 'HAVE_MEMSET', 'HAVE_MEMCPY',
             'HAVE_MEMMOVE', 'HAVE_MEMCMP', 'HAVE_STRLEN', 'HAVE_STRDUP',
             'HAVE_STRCHR', 'HAVE_STRRCHR', 'HAVE_STRSTR', 'HAVE_STRTOL',
             'HAVE_STRTOUL', 'HAVE_STRTOD', 'HAVE_STRCMP', 'HAVE_STRNCMP',
             'HAVE_STRCASECMP', 'HAVE_STRNCASECMP', 'HAVE_SNPRINTF', 'HAVE_VSNPRINTF',
             'SDL_CDROM_DISABLED', 'SDL_JOYSTICK_DISABLED',
             'SDL_THREADS_DISABLED', 'SDL_LOADSO_DISABLED']
    return ('#ifndef SDL_RIPLIX_CONFIG_H\n#define SDL_RIPLIX_CONFIG_H\n' +
            ''.join('#define ' + n + ' 1\n' for n in names) +
            '#define SDL_VIDEO_DRIVER_GX ' + str(int(c16)) + '\n' +
            '#define SDL_VIDEO_DRIVER_DUMMY ' + str(int(not c16)) + '\n' +
            '#define SDL_RIPLIX_C16 ' + str(int(c16)) + '\n' +
            '#define SDL_VIDEO_OPENGL 0\n#define SDL_ASSEMBLY_ROUTINES 0\n' +
            '#define SDL_BYTEORDER 1234\n#endif\n')


def adapt_c16(source):
    def edit(name, fn):
        p = source / name
        p.write_text(fn(p.read_text()))
    # SDL Uint8 is an octet carrier, not a claim that C16 has an exact 8-bit type.
    def standard(t):
        for ty, cells in [('uint16', 1), ('sint16', 1), ('uint32', 2),
                          ('sint32', 2), ('uint64', 4), ('sint64', 4)]:
            t, count = re.subn(r'(SDL_COMPILE_TIME_ASSERT\(' + ty +
                r', sizeof\([^)]*\) == )\d+(\);)',
                lambda m: m[1] + str(cells) + m[2], t)
            assert count == 1
        return t
    edit('include/SDL_stdinc.h', standard)
    # File offsets/counts must not truncate large bitmaps to a signed C16 int.
    def rw(t):
        t = re.sub(r'\bint\b', 'Sint32', t)
        return t
    edit('include/SDL_rwops.h', rw)
    edit('src/file/SDL_rwops.c', rw)
    def memory_streams(t):
        # Clamp using integer positions before pointer arithmetic. An oversized
        # seek is valid SDL behavior; manufacturing an invalid C16 pointer first
        # would trap before the original implementation can clamp it.
        start=t.index('static Sint32 SDLCALL mem_seek(')
        end=t.index('static Sint32 SDLCALL mem_writeconst(',start)
        t=t[:start]+'''static Sint32 SDLCALL mem_seek(SDL_RWops *c, Sint32 offset, Sint32 whence)
{
 Sint32 length=c->hidden.mem.stop-c->hidden.mem.base;
 Sint32 current=c->hidden.mem.here-c->hidden.mem.base, origin, next;
 switch(whence) {
 case RW_SEEK_SET: origin=0;break;
 case RW_SEEK_CUR: origin=current;break;
 case RW_SEEK_END: origin=length;break;
 default: SDL_SetError("Unknown SDL stream seek origin");return -1;
 }
 if(offset < -origin)next=0;
 else if(offset > length-origin)next=length;
 else next=origin+offset;
 c->hidden.mem.here=c->hidden.mem.base+next;return next;
}
static Sint32 SDLCALL mem_read(SDL_RWops *c, void *dst, Sint32 size, Sint32 count)
{
 Uint32 available, bytes;
 if(size<=0 || count<=0)return 0;
 if((Uint32)count > 0xffffffffUL/(Uint32)size) {
  SDL_SetError("SDL stream read size overflows");return 0;
 }
 available=c->hidden.mem.stop-c->hidden.mem.here;
 bytes=(Uint32)count>available/(Uint32)size ? available : (Uint32)count*(Uint32)size;
 SDL_memcpy(dst,c->hidden.mem.here,bytes);c->hidden.mem.here+=bytes;
 return bytes/(Uint32)size;
}
static Sint32 SDLCALL mem_write(SDL_RWops *c, const void *src, Sint32 size, Sint32 count)
{
 Uint32 available, objects, bytes;
 if(size<=0 || count<=0)return 0;
 if((Uint32)count > 0xffffffffUL/(Uint32)size) {
  SDL_SetError("SDL stream write size overflows");return 0;
 }
 available=c->hidden.mem.stop-c->hidden.mem.here;
 objects=available/(Uint32)size;
 if(objects>(Uint32)count)objects=count;
 bytes=objects*(Uint32)size;
 SDL_memcpy(c->hidden.mem.here,src,bytes);c->hidden.mem.here+=bytes;
 return objects;
}
'''+t[end:]
        for name in ['stdio_read','stdio_write']:
            pattern=r'(static Sint32 SDLCALL '+name+r'\([^\n]+\)\n\{)'
            t,count=re.subn(pattern,r'\1\n if (size <= 0 || '+('maxnum' if name=='stdio_read' else 'num')+r' <= 0) return 0;',t)
            assert count==1
        for name in ['SDL_RWFromMem','SDL_RWFromConstMem']:
            pattern=r'('+name+r'\([^\n]+\)\n\{)'
            t,count=re.subn(pattern,r'\1\n if (!mem || size < 0) { SDL_SetError("Invalid SDL memory stream"); return NULL; }',t)
            assert count==1
        return t
    edit('src/file/SDL_rwops.c', memory_streams)
    # Serialized endian helpers read logical octets, never native scalar layout.
    edit('src/file/SDL_rwops.c', lambda t: t[:t.index('Uint16 SDL_ReadLE16')] +
         (PORT / 'c16/endian.inc').read_text())
    # The optimized low-level blitters assume CHAR_BIT=8. Reuse upstream clipping,
    # map lifetime and locking, replacing only the final software pixel operation.
    def blit(t):
        start = t.index('int SDL_CalculateBlit(SDL_Surface *surface)')
        t = t[:start] + '''int SDL_CalculateBlit(SDL_Surface *surface)
{
 surface->flags &= ~(SDL_HWACCEL | SDL_RLEACCEL);
 surface->map->sw_data->blit = SDL_C16CalculateBlit(surface);
 surface->map->sw_blit = SDL_SoftBlit;
 if (!surface->map->sw_data->blit) {
  SDL_SetError("Unsupported C16 software blit"); return -1;
 }
 return 0;
}
'''
        t = t.replace('#include "SDL_blit.h"', '#include "SDL_blit.h"\n#include "pixels.h"')
        t = t.replace('(Uint16)srcrect->y*', '(Uint32)srcrect->y*').replace(
            '(Uint16)dstrect->y*', '(Uint32)dstrect->y*')
        return t
    edit('src/video/SDL_blit.c', blit)
    edit('src/video/SDL_blit.h', lambda t: t.replace('int s_skip;', 'Sint32 s_skip;').replace('int d_skip;', 'Sint32 d_skip;'))
    # GX currently supplies the native arrow but has no cursor-shape/visibility
    # request. Do not run upstream packed-memory software cursor rasterizers.
    edit('src/video/SDL_cursor.c', lambda t: t.replace(
        'void SDL_DrawCursorNoLock(SDL_Surface *screen)\n{',
        'void SDL_DrawCursorNoLock(SDL_Surface *screen)\n{\n return; /* native GX arrow only in this profile */').replace(
        'void SDL_EraseCursorNoLock(SDL_Surface *screen)\n{',
        'void SDL_EraseCursorNoLock(SDL_Surface *screen)\n{\n return; /* native GX arrow only in this profile */'))
    def surface(t):
        t = t.replace('#include "SDL_blit.h"', '#include "SDL_blit.h"\n#include "pixels.h"')
        t = t.replace('surface->h*surface->pitch', '(Uint32)surface->h*surface->pitch')
        t = t.replace('if ( width >= 16384 || height >= 65536 )',
                      'if ( width < 0 || height < 0 || width >= 16384 || height >= 32768UL )')
        t = t.replace('int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)',
                      'int SDL_UpstreamFillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color)')
        t += '\nint SDL_FillRect(SDL_Surface *s, SDL_Rect *r, Uint32 c) { return SDL_C16FillRect(s,r,c); }\n'
        return t
    edit('src/video/SDL_surface.c', surface)
    def pixels(t):
        t = t.replace('#include "SDL_blit.h"', '#include "SDL_blit.h"\n#include "pixels.h"')
        for ch in ['r', 'g', 'b', 'a']:
            t = t.replace('(' + ch + ' >> format->', '((Uint32)' + ch + ' >> format->')
        t = t.replace('0xFF>>', '0xFFUL>>').replace('unsigned int smallest;', 'Uint32 smallest;')
        t = t.replace('unsigned int distance;', 'Uint32 distance;').replace('int rd, gd, bd;', 'Sint32 rd, gd, bd;')
        for ch in ['r','g','b']:
            t = t.replace('= pal->colors[i].'+ch+' - '+ch+';',
                          '= (Sint32)pal->colors[i].'+ch+' - '+ch+';')
        old = '''ASSEMBLE_RGBA(&map[i*bpp], dst->BytesPerPixel, dst,
			      pal->colors[i].r, pal->colors[i].g,
			      pal->colors[i].b, alpha);'''
        t = replace(t, old, '''SDL_C16StorePixel(&map[i*bpp], dst->BytesPerPixel,
            SDL_MapRGBA(dst, pal->colors[i].r, pal->colors[i].g, pal->colors[i].b, alpha));''')
        return t
    edit('src/video/SDL_pixels.c', pixels)
    edit('src/video/SDL_bmp.c', lambda t: t.replace('surface->h*surface->pitch', '(Uint32)surface->h*surface->pitch'))
    edit('src/SDL.c', lambda t: replace(t, 'int SDL_InitSubSystem(Uint32 flags)\n{',
         '''int SDL_InitSubSystem(Uint32 flags)
{
 if (flags & SDL_INIT_EVENTTHREAD) {
  SDL_SetError("SDL event threads are unavailable"); return -1;
 }
'''))
    edit('src/events/SDL_events.c', lambda t: t.replace('#include "SDL_events_c.h"',
        '#include "SDL_events_c.h"\n#include "platform.h"\nextern Uint32 SDL_C16RepeatWait(void);').replace(
        'case 0: SDL_Delay(10);', 'case 0: if (SDL_GX_Wait(SDL_C16RepeatWait()) < 0) return 0; break;'))
    edit('src/events/SDL_keyboard.c', lambda t: t + '''
/* No periodic idle wakeup when repeat is inactive. Unsigned subtraction
 * retains SDL's wraparound tick semantics. */
Uint32 SDL_C16RepeatWait(void)
{
 Uint32 elapsed, target;
 if (!SDL_KeyRepeat.timestamp) return 0xffffffffUL;
 elapsed = SDL_GetTicks() - SDL_KeyRepeat.timestamp;
 target = SDL_KeyRepeat.firsttime ? SDL_KeyRepeat.delay : SDL_KeyRepeat.interval;
 return elapsed > target ? 0 : target - elapsed + 1;
}
''')
    # Event masks exceed the width of int on C16.
    for path in [source / 'include/SDL_events.h', *sorted((source / 'src/events').glob('*.c'))]:
        t = path.read_text()
        t = re.sub(r'\b(0x[0-9A-Fa-f]+|1)\s*<<', r'\1UL <<', t)
        path.write_text(t)
