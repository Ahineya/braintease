#!/usr/bin/env python3
"""Build the NetHack 3.6.7 guest port without touching normal disk images."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from normalize import normalize, cast_opvars

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
FUZIX = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def run(command, **kw):
    return subprocess.run(list(map(str, command)), check=True, **kw)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text: path.write_text(text)


def prepare():
    source = PORT / 'build/source'
    # Pristine sources stay untouched; all generated/modified files live in build/.
    for original in (PORT / 'upstream').rglob('*'):
        if original.is_file():
            destination = source / original.relative_to(PORT / 'upstream')
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists() or destination.read_bytes() != original.read_bytes(): shutil.copy2(original,destination)
    def edit(name, old, new):
        p = source / name
        text = p.read_text()
        if old not in text: raise RuntimeError(f'Missing patch anchor in {name}: {old!r}')
        notice = '/* Modified for Ripple/Fuse-X on 2026-10-19; see ports/nethack/build.py. */\n'
        write(p, ('' if text.startswith(notice) else notice) + text.replace(old, new))
    edit('include/config.h', '#include "global.h"', '''#ifdef RIPPLE_NETHACK
#undef SYSCF
#undef COMPRESS
#undef COMPRESS_EXTENSION
#undef DLB
#undef PANICTRACE
#define NOMAIL
#define NOSHELL
#define NOCWD_ASSUMPTIONS
#undef FCMASK
#define FCMASK 0600
#endif
#ifdef RIPPLE_NETHACK_NX
#undef DEFAULT_WINDOW_SYS
#define DEFAULT_WINDOW_SYS "nano-x"
#endif
#include "global.h"''')
    edit('src/windows.c', '#include "hack.h"', '#include "hack.h"\n#ifdef RIPPLE_NETHACK_NX\nextern struct window_procs nx_procs;\n#endif')
    edit('src/windows.c', '} winchoices[] = {', '} winchoices[] = {\n#ifdef RIPPLE_NETHACK_NX\n    { &nx_procs, 0 },\n#endif')
    edit('include/unixconf.h', '#include "system.h"', '''#ifdef RIPPLE_NETHACK
#undef TERMINFO
#undef SVR4
#undef NETWORK
#undef MAIL
#undef USE_FCNTL
#define PORT_ID "Ripple"
#define DEV_RANDOM "/dev/urandom"
#endif
#include "system.h"''')
    p = source / 'include/system.h'
    write(p, '/* Modified for Ripple/Fuse-X on 2026-10-19. */\n#ifdef RIPPLE_NETHACK\n#include "ripple-system.h"\n#else\n' + p.read_text() + '\n#endif\n')
    for name in ('src/alloc.c', 'include/global.h', 'include/extern.h'):
        edit(name, 'FDECL(alloc, (unsigned int))', 'FDECL(alloc, (size_t))')
    edit('src/alloc.c', 'register unsigned int lth;', 'register size_t lth;')
    edit('src/alloc.c', 'cannot get %u bytes', 'cannot get %lu bytes')
    edit('src/decl.c', 'const char *occtxt = DUMMY;', 'const char *occtxt = 0;')
    edit('src/decl.c', 'time_t ubirthday = DUMMY;', 'time_t ubirthday = 0;')
    edit('src/do_name.c', 'coord *garr = DUMMY;', 'coord *garr = 0;')
    edit('sys/share/unixtty.c', '    case B200:\n        return 6;', '#ifdef B200\n    case B200:\n        return 6;\n#endif')
    edit('sys/share/unixtty.c', '    case B1800:\n        return 10;', '#ifdef B1800\n    case B1800:\n        return 10;\n#endif')
    edit('sys/share/unixtty.c', '#define nonesuch (fpathconf(0, _PC_VDISABLE))', '#define nonesuch _POSIX_VDISABLE')
    edit('sys/share/ioctl.c', '#if defined(BSD) || defined(_AIX32) || defined(__linux__)', '#if defined(BSD) || defined(_AIX32) || defined(__linux__) || defined(RIPPLE_NETHACK)')
    p = source / 'include/sp_lev.h'
    text = p.read_text()
    start = text.index('#define SP_COORD_X(')
    end = text.index('\nstruct splev_var',start)
    write(p,'/* Modified for Ripple/Fuse-X on 2026-10-19. */\n' + text[:start] + '#include "ripple-pack.h"\n' + text[end:])
    edit('win/tty/termcap.c', 'STATIC_VAR char tbuf[512];', 'STATIC_VAR char tbuf[1024];')
    edit('win/tty/wintty.c', '#include "wintty.h"', '#include "wintty.h"\n#include "keys.h"')
    edit('win/tty/wintty.c', 'int\ntty_nhgetch()\n{', 'static int\nripple_tty_getch(const char *directions)\n{')
    edit('win/tty/wintty.c', '? tgetch()', '? nh_terminal_getchar(directions)')
    edit('win/tty/wintty.c', '        i = tgetch();', '        i = nh_terminal_getchar(directions);')
    edit('win/tty/wintty.c', '/*\n * return a key, or 0,', 'int\ntty_nhgetch(void)\n{\n    return ripple_tty_getch((const char *) 0);\n}\n\n/*\n * return a key, or 0,')
    edit('win/tty/wintty.c', '    i = tty_nhgetch();\n#endif /* ?WIN32CON */', '    i = ripple_tty_getch(Cmd.dirchars);\n#endif /* ?WIN32CON */')
    edit('sys/unix/unixmain.c', '    plnamesuffix();', "    plnamesuffix();\n    /* C16 filenames hold 30 cells: leave space for uid and level suffix. */\n    plname[19] = 0;")
    edit('sys/unix/unixmain.c', '    if (dir /* User specified directory? */', '    ripple_nethack_paths(&dir, hname);\n    if (dir /* User specified directory? */')
    edit('include/dlb.h', '#define dlb FILE', '#include \"asset.h\"\n#define dlb nh_asset')
    for operation, function in [('fopen','open'),('fclose','close'),('fread','read'),('fseek','seek'),('fgets','gets'),('fgetc','getc'),('ftell','tell')]:
        edit('include/dlb.h', '#define dlb_' + operation + ' ' + operation, '#define dlb_' + operation + ' nh_asset_' + function)
    for name in ['lev_lex.c','lev_yacc.c','dgn_lex.c','dgn_yacc.c','lev_comp.h','dgn_comp.h']:
        write(source / ('include' if name.endswith('.h') else 'util') / name,
              (PORT / 'upstream/sys/share' / name).read_text())
    # A host makedefs bootstraps symbolic headers only. Binary assets and version
    # layout fingerprints are produced by guest-built tools (see generate.py).
    reference = PORT / 'build/reference'
    for name in ['pm.h', 'onames.h', 'vis_tab.h', 'date.h']:
        origin = reference / 'include' / name
        if not (source / 'include' / name).exists():
            if not origin.exists(): raise RuntimeError('Bootstrap headers missing: run bootstrap.py')
            shutil.copy2(origin, source / 'include' / name)
    if not (source / 'src/vis_tab.c').exists(): shutil.copy2(reference / 'src/vis_tab.c', source / 'src/vis_tab.c')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['fuse-x','fuzix'])
    parser.add_argument('--stage', type=Path, help='stage Fuse-X executable and data in a shared folder')
    parser.add_argument('--group', choices=['game','makedefs','lev_comp','dgn_comp','tilemap'], default='game')
    parser.add_argument('--frontend', choices=['tty','nx'], default='tty')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--prepared', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('-j', '--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.frontend=='nx' and args.group!='game': parser.error('--frontend nx is for the game')
    if args.stage and (args.target != 'fuse-x' or args.group != 'game'): parser.error('--stage requires the Fuse-X game')
    source = PORT / 'build/source' if args.prepared else prepare()
    native = args.target == 'fuse-x'
    graphical=args.frontend=='nx'
    build = PORT / 'build' / (args.target+('-nx' if graphical else ''))
    build.mkdir(parents=True, exist_ok=True)
    includes = [source / 'include', PORT / 'config', FUZIX / 'ripple/include', FUZIX / 'upstream/Library/include', ROOT / 'runtime/include', FUZIX / 'ripple']
    defines = ['RIPPLE_MMU=1','RIPPLE_NETHACK=1','HAS_STDINT_H=1','UNWIDENED_PROTOTYPES=1', 'FUSE_X_USER=1' if native else 'RIPPLE=1', 'HACKDIR="/usr/share/nethack"']
    external={}
    if graphical:
        nx=ROOT/'ports/nanox'
        generated=ROOT/('fuse-x/build/nanox-native/src' if native else 'build/fuzix/nanox/src')
        run(['python3',nx/'adapt.py',generated],stdout=subprocess.DEVNULL)
        if native: run(['python3',ROOT/'fuse-x/nanox/adapt.py',generated],stdout=subprocess.DEVNULL)
        includes += [generated/'include',generated/'drivers',generated/'nanox',nx/'ripple',ROOT/'ports/graphics/include',ROOT/'ports/fusede/include',ROOT/('fuse-x/build/fusede' if native else 'build/fuzix/fusede')]
        defines += ['RIPPLE_NETHACK_NX=1','MWCONFIG_FILE="nxr_config.h"','MWPIXEL_FORMAT=MWPF_TRUECOLOR565','NXR_DESKTOP=1']
        for name in ['client','nxproto','nxdraw','nxpaintnc','nxutil']:
            external['nx-client/'+name+'.c']=generated/'nanox'/(name+'.c')
        external['nx-client/client-os.c']=nx/'ripple/client-os.c'
        external['nx-client/skin.c']=ROOT/'ports/fusede/skin.c'
        external['nx/window.c']=PORT/'nx/window.c'
        external['nx/ui.c']=PORT/'nx/ui.c'
        external['nx/tile.c']=PORT/'build/nx-assets/tile.c'
        if not external['nx/tile.c'].exists(): raise RuntimeError('Run tiles.py before building the Nano-X game')
    names = re.search(r'^HACKCSRC = (.*?)(?=\n\n)', (source / 'sys/unix/Makefile.src').read_text(), re.M|re.S)[1].replace('\\\n',' ').split()
    if args.group == 'game':
        paths = ['src/' + name for name in names] + ['src/vis_tab.c','sys/share/ioctl.c','sys/share/unixtty.c','sys/share/pmatchregex.c','sys/unix/unixmain.c','sys/unix/unixunix.c','sys/unix/unixres.c'] + ['win/tty/' + n + '.c' for n in ['getline','termcap','topl','wintty']]
        entry = 'sys/unix/unixmain.c'
        paths += ['asset.c','paths.c','keys.c']
        if graphical: paths += list(external)
    elif args.group == 'makedefs':
        paths = ['util/makedefs.c','src/monst.c','src/objects.c']
        entry = paths[0]
    elif args.group == 'lev_comp':
        paths = ['util/lev_main.c','util/lev_lex.c','util/lev_yacc.c','src/alloc.c','util/panic.c','src/drawing.c','src/decl.c','src/monst.c','src/objects.c']
        entry = paths[0]
    elif args.group=='dgn_comp':
        paths = ['util/dgn_main.c','util/dgn_lex.c','util/dgn_yacc.c','src/alloc.c','util/panic.c']
        entry = paths[0]
    else:
        paths=['win/share/tilemap.c']
        entry=paths[0]
    paths.append('compat.c')
    compiler = ROOT / 'target/release/rcc'
    flags = ['--bank-size','64000'] + (['--target','native','--emit-ir'] if native else [])
    ppflags = ['-E','-P','-undef','-nostdinc'] + ['-I'+str(i) for i in includes] + ['-D'+d for d in defines]
    identity = str((compiler.stat().st_mtime_ns, flags, ppflags, (PORT / 'normalize.py').read_text())).encode()
    def compile_one(name):
        input_path = external[name] if name in external else PORT / 'config' / name if name in ['compat.c','asset.c','paths.c','keys.c'] else source / name
        stem = name.replace('/','_')[:-2]
        directory = build / 'objects'
        directory.mkdir(exist_ok=True)
        output = directory / (stem + ('.ir' if native else '.pobj'))
        signature = output.with_suffix('.sha256')
        digest = hashlib.sha256(identity)
        for dep in c_dependencies(input_path, includes):
            digest.update(str(dep).encode()); digest.update(dep.read_bytes())
        key = digest.hexdigest()
        ansi = directory / (stem + '.c')
        if output.exists() and ansi.exists() and signature.exists() and signature.read_text() == key:
            return output, ansi
        print(f'{args.target}: {name}', flush=True)
        pp = subprocess.run(['clang', *ppflags, str(input_path)], capture_output=True, text=True)
        if pp.returncode: return None, str(input_path) + ':\n' + pp.stderr
        write(ansi, cast_opvars(normalize(pp.stdout)))
        assembly = output.with_suffix('.asm')
        result = subprocess.run(list(map(str,[compiler,'compile',ansi,'--no-preprocess',*flags] + ([] if native else ['-o',assembly]))),capture_output=True)
        if result.returncode:
            output.with_suffix('.log').write_bytes(result.stderr)
            return None, str(input_path) + ':\n' + result.stderr.decode(errors='replace')[-3000:]
        if native: output.write_bytes(result.stdout)
        else: run([ROOT / 'src/ripple-asm/target/release/rasm','assemble',assembly,'-o',output,'--bank-size','64000','--max-immediate','65535'],stdout=subprocess.DEVNULL)
        signature.write_text(key)
        return output, ansi
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(compile_one,paths))
    errors = [r[1] for r in results if r[0] is None]
    if errors: raise RuntimeError('\n\n'.join(errors))
    if args.compile_only: return
    output = build / (('nethack-nx' if graphical else 'nethack' if args.group == 'game' else args.group) + ('.fxe' if native else '.rxe'))
    objects = [r[0] for name,r in zip(paths,results) if not native or name != entry]
    if native:
        ansi = results[paths.index(entry)][1]
        command = ['python3',ROOT/'fuse-x/cc.py',ansi,'--static','-o',output]
        for obj in objects: command += ['--object-ir',obj]
        run(command)
    else:
        sdk = ROOT/'build/fuzix'
        softfloat = sdk / 'awk/softfloat.pobj'
        run(['make','-C',FUZIX,f'BUILD={sdk}',f'-j{args.jobs}','rfl-sdk',softfloat])
        info = json.loads(run(['make','-s','-C',FUZIX,f'BUILD={sdk}','rfl-sdk-info'],capture_output=True,text=True).stdout)
        run([ROOT/'src/ripple-asm/target/release/rlink',info['entry'],*objects,softfloat,*info['libraries'],'-f','rxe','--bank-size','64000','-o',output])
    if args.stage:
        from stage import stage
        stage(args.stage)
    print(f'Built {output}', flush=True)

if __name__ == '__main__':
    try: main()
    except RuntimeError as error: sys.exit(str(error))
    except subprocess.CalledProcessError as error: sys.exit(f'Build command failed (status {error.returncode}); see diagnostics above.')
