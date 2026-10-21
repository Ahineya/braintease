#!/usr/bin/env python3
"""Build the existing GX terminal and Nano-X sources for hosted native execution."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
NX = ROOT / 'ports/nanox'
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--only', nargs='+', help='build only these named graphics programs')
    args = parser.parse_args()
    from memory_mode import selected as native_memory_mode
    try:
        memory_mode = native_memory_mode()
    except ValueError as error:
        parser.error(str(error))
    build, compiler = args.build.resolve(), args.compiler.resolve()
    generated = build / 'nanox-native/src'
    assets = build / 'fusede'
    subprocess.run([sys.executable, str(ROOT / 'ports/fusede/tools/build_theme.py'), '--output', str(assets)], check=True)
    subprocess.run([sys.executable, str(ROOT / 'ports/fusede/tools/build_icons.py'), '--output', str(assets)], check=True)
    subprocess.run([sys.executable, str(NX / 'adapt.py'), str(generated)], check=True)
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/nanox/adapt.py'), str(generated)], check=True)
    # The shared puzzle collection is an explicit --only build, never part of
    # the OS graphics build. Do not generate or fingerprint its headers here.
    puzzle_names = {game['id'] for game in json.loads((ROOT / 'ports/puzzles/catalog.json').read_text())}
    build_puzzles = bool(args.only and set(args.only) & (puzzle_names | {'puzzles'}))
    puzzles = None
    if build_puzzles:
        sys.path.insert(0, str(ROOT / 'ports/puzzles'))
        from adapt import generate as generate_puzzles, COMMON as PUZZLE_COMMON, EXTRA as PUZZLE_EXTRA
        puzzles = generate_puzzles(build / 'puzzles/src')
    apps = build / 'apps'
    apps.mkdir(parents=True, exist_ok=True)
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
             '-DMWCONFIG_FILE="nxr_config.h"', '-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565', '-DNXR_DESKTOP=1']
    heap_profile=os.environ.get('FUSE_X_HEAP_PROFILE','hosted')
    if heap_profile not in ('hosted','bounded'):parser.error('FUSE_X_HEAP_PROFILE must be hosted or bounded')
    if heap_profile=='bounded':flags+=['-DFUSE_X_BOUNDED_HEAP=1']
    includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include', ROOT / 'runtime/include',
                PORT / 'ripple', ROOT / 'ports/graphics/include', generated / 'include',
                generated / 'drivers', ROOT / 'fuse-x/nanox', generated / 'nanox', generated / 'demos/nanox', NX / 'ripple',
                ROOT / 'ports/fusede/include', assets]
    if puzzles is not None:
        includes.append(puzzles)
    for path in includes:
        flags += ['-I', str(path)]
    digest = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for path in includes:
        for header in sorted(path.rglob('*.h')):
            digest.update(header.read_bytes())
    stamp = digest.digest()
    sdk = hashlib.sha256(Path(__file__).read_bytes() + (ROOT / 'fuse-x/cc.py').read_bytes())
    sdk.update(memory_mode.encode())
    sdk.update(os.environ.get('FUSE_X_SHARED_LIBRARIES', '1').encode())
    for path in [PORT / 'ripple/libc', PORT / 'upstream/Library/libs', PORT / 'ripple/include', PORT / 'upstream/Library/include', ROOT / 'fuse-x/libc', ROOT / 'fuse-x/include', ROOT / 'runtime/include', ROOT / 'libraries/unicode']:
        for file in sorted(path.rglob('*')):
            if file.suffix in ('.c', '.h', '.inc'):
                sdk.update(file.read_bytes())
    sdk.update((ROOT / 'ports/graphics/raster-check.inc').read_bytes())
    sdk.update((PORT / 'ripple/userlib.c').read_bytes())
    sdk.update((ROOT / 'target/release/librcc_native_runtime.a').read_bytes())

    force = os.environ.get('RIPPLE_REBUILD') == '1'

    def compile_object(item):
        source, extra = item
        unit_flags = [f for f in flags if f != '-DNO_64BIT'] if source.parent in (ROOT / 'ports/kolf', ROOT / 'ports/puzzles', puzzles) else flags
        digest = hashlib.sha256(stamp + str((source, extra, unit_flags)).encode())
        for dependency in c_dependencies(source, includes):
            digest.update(str(dependency).encode());digest.update(dependency.read_bytes())
        key = digest.hexdigest()[:24]
        output = build / 'nanox-native' / f'{source.stem}-{key}.ir'
        if not output.exists():
            result = subprocess.run([str(compiler), 'compile', str(source), *unit_flags, *extra, '--emit-ir'], capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{source}:\n{result.stderr.decode(errors="replace")}')
            output.write_bytes(result.stdout)
        return output

    engine = [generated / f'engine/{name}.c' for name in 'devopen devdraw devclip devrgn devblit devfont'.split()]
    engine += [generated / 'drivers/genfont.c', generated / 'fonts/compiled/rom8x16.c', NX / 'ripple/screen.c']
    server = [generated / f'nanox/{name}.c' for name in 'srvfunc srvutil srvevent srvclip srvnet'.split()]
    server += [generated / f'engine/{name}.c' for name in 'devmouse devkbd devtimer'.split()]
    server += [generated / 'native-server.c', ROOT / 'fuse-x/nanox/backend.c', generated / 'nanox/nxdraw.c',
               NX / 'ripple/image-server.c', NX / 'ripple/workspace-server.c', NX / 'ripple/clipboard-server.c', ROOT / 'ports/fusede/raster.c']
    native_engine = [generated / 'native-screen.c' if p == NX / 'ripple/screen.c' else p for p in engine]
    client = [generated / f'nanox/{name}.c' for name in 'client nxproto nxdraw nxpaintnc nxutil'.split()]
    client += [NX / 'ripple/client-os.c', ROOT / 'ports/fusede/skin.c']
    if not args.only or 'nxview' in args.only:
        subprocess.run([sys.executable, str(ROOT / 'libraries/image/adapt.py')], check=True)
    image = [ROOT / 'libraries/image/decode.c', ROOT / 'libraries/image/nano.c']
    wm = [generated / f'nanox/{name}.c' for name in 'wmaction wmclients wmevents wmutil'.split()]
    profiles = [
        ('nano-X', generated / 'nanox/srvmain.c', native_engine + server, ['-DNXR_SERVER=1', '-DNXR_HOSTED=1']),
        ('nanowm', generated / 'demos/nanox/nanowm.c', wm + client, []),
        ('nxlaunch', generated / 'demos/nanox/nxlaunch.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxterm', generated / 'native-nxterm.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxclock', generated / 'demos/nanox/nxclock.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxeyes', generated / 'demos/nanox/nxeyes.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxmsg', generated / 'demos/nanox/nxmsg.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxselect', generated / 'demos/nanox/nxselect.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxcalc', generated / 'demos/nanox/nxcalc.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxtetris', generated / 'demos/nanox/nxtetris.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxview', NX / 'ripple/nxview.c', client + image, ['-DNXR_NATIVE_GUI=1']),
        ('kolf', ROOT / 'ports/kolf/main.c', client + [ROOT / 'ports/kolf/engine.c', ROOT / 'ports/kolf/putting.c', ROOT / 'ports/kolf/course.c', ROOT / 'libraries/ini/ini.c', ROOT / 'libraries/deflate/deflate.c'], ['-DNXR_NATIVE_GUI=1']),
        ('klotski', ROOT / 'ports/klotski/main.c', client + [ROOT / 'ports/klotski/engine.c', ROOT / 'ports/klotski/store.c'], ['-DNXR_NATIVE_GUI=1']),
        ('shikaku', ROOT / 'ports/shikaku/main.c', client + [ROOT / 'ports/shikaku/engine.c', ROOT / 'ports/shikaku/store.c', ROOT / 'ports/shikaku/generator.c'], ['-DNXR_NATIVE_GUI=1']),
        ('nxcheck', NX / 'ripple/nxcheck.c', client, []),
        ('nxhostcheck', ROOT / 'fuse-x/nanox/check.c', client, []),
        ('fdeshowcase', ROOT / 'ports/fusede/showcase.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('fdectl', ROOT / 'ports/fusede/workspace.c', client, []),
        ('startx', ROOT / 'fuse-x/nanox/startx.c', [], []),
        ('nxdraw', NX / 'ripple/nxdraw.c', engine, ['-DNXR_SERVER=0']),
        ('gterm', PORT / 'ripple/gterm.c', [], []),
        ('gxcheck', ROOT / 'fuse-x/examples/graphics.c', [], []),
    ]
    if build_puzzles:
        profiles.append(('puzzles', ROOT / 'ports/puzzles/launcher.c', client, ['-DNXR_NATIVE_GUI=1']))
        for name in sorted(puzzle_names):
            sources = [puzzles / f'{unit}.c' for unit in [*PUZZLE_COMMON, *PUZZLE_EXTRA[name], name]]
            profiles.append((name, ROOT / 'ports/puzzles/frontend.c', client + sources + [ROOT / 'ports/puzzles/platform.c'], ['-DNXR_NATIVE_GUI=1', '-DNO_TGMATH_H=1']))
    if args.only and set(args.only) - {p[0] for p in profiles}:
        parser.error('unknown graphics profile')
    for name, main_source, sources, defines in profiles:
        if args.only and name not in args.only:
            continue
        with ThreadPoolExecutor(max_workers=4) as pool:
            objects = list(pool.map(compile_object, [(source, defines + (['-DGdBlit=nxr_engine_blit'] if source.stem == 'devblit' else [])) for source in sources]))
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(main_source), '--build', str(build), '--compiler', str(compiler), '-o', str(apps / f'{name}.fxe')]
        for include in includes:
            command += ['-I', str(include)]
        for define in ['MWCONFIG_FILE="nxr_config.h"', 'MWPIXEL_FORMAT=MWPF_TRUECOLOR565', 'NXR_DESKTOP=1', *[d[2:] for d in defines]]:
            command += ['-D', define]
        for obj in objects:
            command += ['--object-ir', str(obj)]
        if name in ('nxeyes', 'nxcalc', 'kolf') or name in puzzle_names:
            command += ['--float-format']
        if name in ('kolf', 'klotski', 'shikaku', 'puzzles') or name in puzzle_names:
            # Also run on images built before the shared-library SDK existed.
            command += ['--static']
        key = hashlib.sha256(stamp + sdk.digest() + main_source.read_bytes() + str(command).encode())
        for obj in objects:
            key.update(obj.read_bytes())
        for dependency in c_dependencies(main_source, includes):
            key.update(str(dependency).encode());key.update(dependency.read_bytes())
        manifest = build / 'nanox-native' / f'{name}.sha256'
        if not force and (apps / f'{name}.fxe').exists() and manifest.exists() and manifest.read_text() == key.hexdigest():
            continue
        subprocess.run(command, check=True)
        manifest.write_text(key.hexdigest())


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
