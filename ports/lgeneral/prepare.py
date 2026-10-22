"""Reproducible, separately generated LGeneral sources; no upstream edits."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import urllib.request
from abi import adapt as abi_adapt
from engine_adapt import adapt as engine_adapt
from graphics_adapt import adapt as graphics_adapt
from terrain_adapt import adapt as terrain_adapt

PORT=Path(__file__).resolve().parent
ROOT=PORT.parents[1]
UNITS='main config date file map nation player scenario lg-sdl misc unit audio parser list unit_lib terrain image slot gui purchase_dlg event windows action campaign strat_map ai engine ai_group ai_tools'.split()
UTIL=['hashtable','hashtable_itr','hashtable_utility','paths','portability','localize']


def replace(s,old,new):
    if s.count(old)!=1:raise RuntimeError('LGeneral source adaptation changed: '+old[:100])
    return s.replace(old,new)


def archives(build,fetch=False):
    pins=json.loads((PORT/'PROVENANCE.json').read_text());result={}
    for key,pin in pins.items():
        archive=ROOT/'build/downloads'/pin['archive'];archive.parent.mkdir(parents=True,exist_ok=True)
        if not archive.exists():
            if not fetch:raise RuntimeError('Missing '+str(archive)+'; use --fetch')
            tmp=archive.with_suffix('.download')
            with urllib.request.urlopen(pin['url'],timeout=60) as response:tmp.write_bytes(response.read())
            if hashlib.sha256(tmp.read_bytes()).hexdigest()!=pin['sha256']:raise RuntimeError(key+' checksum mismatch')
            tmp.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=pin['sha256']:raise RuntimeError(key+' checksum mismatch')
        folder=build/'pristine'/key;folder.mkdir(parents=True,exist_ok=True)
        with tarfile.open(archive) as tar:tar.extractall(folder,filter='data')
        result[key]=next(p for p in folder.iterdir() if p.is_dir())
    return result


def prepare(build,fetch=False,c16=True):
    origins=archives(build,fetch)
    source=build/('c16' if c16 else 'host')
    shutil.copytree(origins['engine'],source,dirs_exist_ok=True)
    for path in [*source.rglob('*.c'),*source.rglob('*.h')]:
        path.write_text(path.read_bytes().decode('latin1'))
    (source/'config.h').write_text('''#ifndef LG_BUILD_CONFIG_H
#define LG_BUILD_CONFIG_H
#define VERSION "1.4.4"
#define PACKAGE "lgeneral"
#define PREFIX "/usr"
#define INSTALLDIR "/usr/share/lgeneral"
#define PATHS_GENERIC 1
#define HAVE_CONFIG_H 1
#define HAVE_SETENV 1
#define HAVE_PUTENV 1
#define HAVE_STPCPY 1
#define HAVE_STRNLEN 1
#define HAVE_STRTOUL 1
#define HAVE_STRCASECMP 1
#define NODIM 1
#define __FUNCTION__ "lgeneral"
#endif
''')
    # The English release resources use bitmap fonts. Catalog/gettext support is
    # optional and independent of engine parsing and campaign logic.
    path=source/'util/localize.h';path.write_text(path.read_text().replace('#include <libintl.h>',''))
    shutil.copy2(PORT/'platform/localize.c',source/'util/localize.c')
    shutil.copy2(PORT/'platform/cli.h',source/'src/cli.h')
    engine_adapt(source,c16);graphics_adapt(source,c16);terrain_adapt(source,c16)
    from save_adapt import adapt as save_adapt
    save_adapt(source,c16)
    path=source/'src/misc.c';s=path.read_text();a=s.index('const char *get_gamedir(void)')
    s=s[:a]+'''const char *lg_data_directory;
const char *get_gamedir(void)
{
    extern const char *lg_data_directory;
    const char *value = lg_data_directory ? lg_data_directory : getenv("LGENERAL_DATA");
    return value && *value ? value : INSTALLDIR;
}
''';path.write_text(s)
    path=source/'src/config.c';s=path.read_text().replace('getenv( "HOME" )','(getenv("HOME") ? getenv("HOME") : "/tmp")');path.write_text(s)
    path=source/'src/main.c';s=path.read_text().replace('#include <getopt.h>','#include "cli.h"\n#include <sys/stat.h>')
    if c16:
        s=replace(s, '    /* init sdl */', '''    /* Legacy bitmap artwork is more readable in a zoomed native window.
     * This affects this application only; callers can override the hint. */
    if (!getenv("SDL_GX_WINDOW_ZOOM")) putenv("SDL_GX_WINDOW_ZOOM=150");
    /* A staged game and its PCM helper travel together. Installed packages
     * instead resolve the normal /usr/bin or /bin helper. */
    if (!getenv("SDL_MIXER_HELPER")) {
        static char helper[600];
        const char *slash = strrchr(argv[0], '/');
        struct stat info;
        if (slash && (size_t)(slash-argv[0])+32 < sizeof(helper)) {
            strcpy(helper, "SDL_MIXER_HELPER=");
            memcpy(helper+17, argv[0], (size_t)(slash-argv[0]));
            strcpy(helper+17+(slash-argv[0]), "/sdl-mixerd.fxe");
            if (stat(helper+17, &info)==0 && S_ISREG(info.st_mode)) putenv(helper);
        }
    }
    /* init sdl */''')
    s=s.replace('    eval_cmdline(argc, argv);','''    /* Fullscreen is not a GX window capability. */
    config.fullscreen = 0;
    if (config.width < 800 || config.width > 1920) config.width = 800;
    if (config.height < 600 || config.height > 1080) config.height = 600;
    eval_cmdline(argc, argv);
    if (config.anim_speed < 1) abortf("Animation speed must be positive\\n");''')
    s=replace(s,'    locale_init(0);','''    /* Strip the data option before upstream scenario/campaign option parsing. */
    {
        extern const char *lg_data_directory;
        int i=1, out=1;
        while (i<argc) {
            if (!strcmp(argv[i], "--data-dir")) {
                if (++i==argc) { fprintf(stderr,"--data-dir needs a directory\\n"); return 1; }
                lg_data_directory=argv[i++];
            } else if (!strncmp(argv[i], "--data-dir=", 11)) {
                lg_data_directory=argv[i++]+11;
            } else argv[out++]=argv[i++];
        }
        argc=out; argv[out]=0;
        /* A staged executable can find the adjacent read-only asset folder. */
        if (!lg_data_directory && !getenv("LGENERAL_DATA")) {
            static char nearby[512];
            const char *slash=strrchr(argv[0], '/');
            struct stat info;
            if (slash && (size_t)(slash-argv[0])+9 < sizeof nearby) {
                memcpy(nearby,argv[0],(size_t)(slash-argv[0]));
                strcpy(nearby+(slash-argv[0]),"/lgeneral");
                if (stat(nearby,&info)==0 && S_ISDIR(info.st_mode)) lg_data_directory=nearby;
            }
        }
    }
    locale_init(0);''')
    s=s.replace('    engine_run();','''#ifdef LGENERAL_ENGINE_TEST
    { extern int lgeneral_engine_test(void); int result=lgeneral_engine_test();
      engine_delete(); return result; }
#else
    engine_run();
#endif''')
    path.write_text(s)
    # Sound assets can fail independently of rendering. The upstream wrapper
    # dereferences the freed/NULL Wav on load failure; keep playback optional.
    path=source/'src/audio.c';text=path.read_text()
    # Keep upstream's short 256-frame blocks. The process producer continues
    # filling them while the application composes bitmap dialogs.
    text=replace(text,'    Mix_CloseAudio();','    Mix_CloseAudio();\n    audio_ok = 0;')
    text=replace(text,'        Mix_Pause( -1 ); /* stop all sound channels */',
                 '        Mix_Pause(-1);\n    else\n        Mix_Resume(-1);')
    text=replace(text,'    if ( !audio_ok ) return 0;','    if (!audio_ok || !fname) return 0;')
    text=replace(text,'    wav = calloc( 1, sizeof( Wav ) );',
                 '    wav = calloc(1, sizeof(Wav));\n    if (!wav) return 0;')
    text=replace(text,'    sprintf( path, "%s/sounds/%s", get_gamedir(), fname );',
                 '''    if (snprintf(path, sizeof(path), "%s/sounds/%s", get_gamedir(), fname) >= sizeof(path)) {
        fprintf(stderr, "Sound asset pathname is too long\\n");
        free(wav); return 0;
    }''')
    text=replace(text,'        free( wav ); wav = 0;','        free(wav); return 0;')
    text=replace(text,'    wav->channel = channel;\n    return wav;',
                 '''    wav->channel = channel;
    if (getenv("LGENERAL_TRACE"))
        fprintf(stderr, "LGENERAL SOUND %s %lu octets\\n", fname, (unsigned long)wav->chunk->alen);
    return wav;''')
    text=text.replace('    wav->channel = channel;', '    if (!wav) return;\n    wav->channel = channel;')
    # wav_load returns a pointer, so its checked successful assignment must not
    # inherit the void helper guard inserted above.
    text=replace(text,'    if (!wav) return;\n    wav->channel = channel;\n    if (getenv("LGENERAL_TRACE"))',
                 '    wav->channel = channel;\n    if (getenv("LGENERAL_TRACE"))')
    text=replace(text,'    if ( !audio_enabled ) return;','    if (!audio_enabled || !wav) return;')
    text=replace(text,'    Mix_FadeOutChannel( wav->channel, ms );',
                 '    if (wav) Mix_FadeOutChannel(wav->channel, ms);')
    path.write_text(text)
    path=source/'src/audio.h';text=path.read_text()
    text=replace(text,'void wav_play_at( Wav *wav, int channel );',
                 'void wav_play_at( Wav *wav, int channel );\nvoid wav_fade_out(Wav *wav, int ms);')
    path.write_text(text)
    # SDL's event filter handles keys/mouse but queues resize events for the
    # engine. Its C int pointer parameter must not alias a 32-bit game integer.
    path=source/'src/engine.c';s=path.read_text().replace('SDL_GetKeyState(&numkeys)','lg_get_key_state(&numkeys)')
    s=s.replace('#include "lgeneral.h"','#include "lgeneral.h"\nstatic Uint8 *lg_get_key_state(int *count) { int16_t n; Uint8 *keys=SDL_GetKeyState(&n); *count=n; return keys; }') if c16 else s.replace('lg_get_key_state(&numkeys)','SDL_GetKeyState(&numkeys)')
    path.write_text(s)
    # Native desktop windows are resizable; use the engine's existing video
    # mode action so map buffers, minimap and dialogs are rebuilt together.
    path=source/'src/lg-sdl.c';text=path.read_text()
    text=text.replace('    int depth = 32;', '    int depth = 24;')
    text=text.replace('    int flags = SDL_SWSURFACE;', '    int flags = SDL_SWSURFACE | SDL_RESIZABLE;')
    text=text.replace('SDL_SetVideoMode( 800, 600, 16, SDL_SWSURFACE )', 'SDL_SetVideoMode(800, 600, 24, SDL_SWSURFACE | SDL_RESIZABLE)')
    # Offer windowed sizes only: fullscreen is not provided by the GX driver.
    start=text.index('int get_video_modes( VideoModeInfo **vmi )')
    # Replace the complete function, leaving its following section comment.
    function_end=text.index('\n}\n',start)+3
    text=text[:start]+'''int get_video_modes(VideoModeInfo **vmi)
{
    static const VideoModeInfo sizes[] = {
        {800,600,24,0}, {1024,768,24,0}, {1280,720,24,0}, {1600,900,24,0}
    };
    *vmi=malloc(sizeof sizes);
    if (!*vmi) return 0;
    memcpy(*vmi,sizes,sizeof sizes);
    return sizeof sizes / sizeof sizes[0];
}
'''+text[function_end:]
    path.write_text(text)
    path=source/'src/engine.c';text=path.read_text()
    text=replace(text,'            if ( event.type == SDL_KEYDOWN )','''            if (event.type == SDL_VIDEORESIZE) {
                int w=event.resize.w, h=event.resize.h;
                if (w<800) w=800; if (h<600) h=600;
                if (w>1920) w=1920; if (h>1080) h=1080;
                action_queue_set_vmode(w,h,0);
            }
            if ( event.type == SDL_KEYDOWN )''')
    text=replace(text,'    gui_get_bkgnds();\n    *reinit = 0;','''    gui_get_bkgnds();
    if (getenv("LGENERAL_TRACE"))
        fprintf(stderr,"LGENERAL READY %s %s\\n",
            setup.type==SETUP_RUN_TITLE ? "title" :
            (setup.type==SETUP_CAMP_BRIEFING ? "campaign" : "scenario"),setup.fname);
    *reinit = 0;''')
    text=text.replace('            flags = SDL_SWSURFACE;', '            flags = SDL_SWSURFACE | SDL_RESIZABLE;')
    text=text.replace('SDL_VideoModeOK( action->w, action->h, 32, flags )','SDL_VideoModeOK( action->w, action->h, 24, flags )')
    text=text.replace('                SDL_SetVideoMode( action->w, action->h, depth, flags );','''                SDL_Surface *resized=SDL_SetVideoMode(action->w,action->h,depth,flags);
                if (!resized) { fprintf(stderr,"Resize failed: %s\\n",SDL_GetError()); break; }
                sdl.screen=resized;''')
    path.write_text(text)
    path=source/'src/event.c';text=path.read_text()
    text=replace(text,'            buttonstate[event->motion.state] = 1;','''            { int i; for (i=1;i<10;i++)
                buttonstate[i]=(event->motion.state & (1U << (i-1))) != 0; }''')
    path.write_text(text)
    path=source/'util/hashtable_itr.h';text=path.read_text()
    text=text.replace('extern inline', 'static inline')
    path.write_text(text)
    path=source/'util/hashtable_itr.c';text=path.read_text()
    start=text.index('void *\nhashtable_iterator_key(')
    end=text.index('/*****************************************************************************',start)
    text=text[:start]+text[end:]
    path.write_text(text)
    # GNU89 inline declarations are exported functions, not C99 extern-inline.
    for name in ('lg-sdl','misc'):
        for suffix in ('.c','.h'):
            path=source/'src'/(name+suffix)
            path.write_text(re.sub(r'\binline\s+', '',path.read_text()))
    abi_adapt(source,c16)
    changed={str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(source.rglob('*')) if p.is_file() and p.suffix in ('.c','.h','.inc') and (not (origins['engine']/p.relative_to(source)).exists() or p.read_bytes()!=(origins['engine']/p.relative_to(source)).read_bytes())}
    (build/('adaptation-c16.json' if c16 else 'adaptation-host.json')).write_text(json.dumps(dict(upstream=json.loads((PORT/'PROVENANCE.json').read_text()),files=changed),indent=2)+'\n')
    return source,origins
