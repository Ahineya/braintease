#!/usr/bin/env python3
"""Build the existing GX terminal and Nano-X sources for hosted native execution."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
NX = ROOT / 'ports/nanox'
PORT = ROOT / 'ports/fuzix'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    args = parser.parse_args()
    build, compiler = args.build.resolve(), args.compiler.resolve()
    generated = build / 'nanox-native/src'
    subprocess.run([sys.executable, str(NX / 'adapt.py'), str(generated)], check=True)
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/nanox/adapt.py'), str(generated)], check=True)
    apps = build / 'apps'
    apps.mkdir(parents=True, exist_ok=True)
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
             '-DMWCONFIG_FILE="nxr_config.h"', '-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565', '-DNXR_DESKTOP=1']
    includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include', ROOT / 'runtime/include',
                PORT / 'ripple', ROOT / 'ports/graphics/include', generated / 'include',
                generated / 'drivers', ROOT / 'fuse-x/nanox', generated / 'nanox', generated / 'demos/nanox', NX / 'ripple']
    for path in includes:
        flags += ['-I', str(path)]
    digest = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for path in includes:
        for header in sorted(path.rglob('*.h')):
            digest.update(header.read_bytes())
    # Include implementation fragments (e.g. devclip2.c and requests.inc),
    # which the compiler reads through textual includes in another unit.
    for directory in [generated, NX / 'ripple']:
        for file in sorted(directory.rglob('*')):
            if file.suffix in ('.c', '.h', '.inc'):
                digest.update(file.read_bytes())
    stamp = digest.digest()
    sdk = hashlib.sha256(Path(__file__).read_bytes() + (ROOT / 'fuse-x/cc.py').read_bytes())
    for path in [PORT / 'ripple/libc', PORT / 'upstream/Library/libs', PORT / 'ripple/include', PORT / 'upstream/Library/include', ROOT / 'fuse-x/libc', ROOT / 'runtime/include']:
        for file in sorted(path.rglob('*')):
            if file.suffix in ('.c', '.h'):
                sdk.update(file.read_bytes())
    sdk.update((PORT / 'ripple/userlib.c').read_bytes())

    def compile_object(item):
        source, extra = item
        key = hashlib.sha256(stamp + source.read_bytes() + str((source, extra)).encode()).hexdigest()[:24]
        output = build / 'nanox-native' / f'{source.stem}-{key}.ir'
        if not output.exists():
            result = subprocess.run([str(compiler), 'compile', str(source), *flags, *extra, '--emit-ir'], capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{source}:\n{result.stderr.decode(errors="replace")}')
            output.write_bytes(result.stdout)
        return output

    engine = [generated / f'engine/{name}.c' for name in 'devopen devdraw devclip devrgn devblit devfont'.split()]
    engine += [generated / 'drivers/genfont.c', generated / 'fonts/compiled/rom8x16.c', NX / 'ripple/screen.c']
    server = [generated / f'nanox/{name}.c' for name in 'srvfunc srvutil srvevent srvclip srvnet'.split()]
    server += [generated / f'engine/{name}.c' for name in 'devmouse devkbd devtimer'.split()]
    server += [generated / 'native-server.c', ROOT / 'fuse-x/nanox/backend.c', generated / 'nanox/nxdraw.c']
    native_engine = [generated / 'native-screen.c' if p == NX / 'ripple/screen.c' else p for p in engine]
    client = [generated / f'nanox/{name}.c' for name in 'client nxproto nxdraw nxpaintnc nxutil'.split()]
    client += [NX / 'ripple/client-os.c']
    wm = [generated / f'nanox/{name}.c' for name in 'wmaction wmclients wmevents wmutil'.split()]
    profiles = [
        ('nano-X', generated / 'nanox/srvmain.c', native_engine + server, ['-DNXR_SERVER=1']),
        ('nanowm', generated / 'demos/nanox/nanowm.c', wm + client, []),
        ('nxlaunch', generated / 'demos/nanox/nxlaunch.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxterm', generated / 'native-nxterm.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxclock', generated / 'demos/nanox/nxclock.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxeyes', generated / 'demos/nanox/nxeyes.c', client, ['-DNXR_NATIVE_GUI=1']),
        ('nxcheck', NX / 'ripple/nxcheck.c', client, []),
        ('nxhostcheck', ROOT / 'fuse-x/nanox/check.c', client, []),
        ('startx', ROOT / 'fuse-x/nanox/startx.c', [], []),
        ('nxdraw', NX / 'ripple/nxdraw.c', engine, ['-DNXR_SERVER=0']),
        ('gterm', PORT / 'ripple/gterm.c', [], []),
        ('gxcheck', ROOT / 'fuse-x/examples/graphics.c', [], []),
    ]
    for name, main_source, sources, defines in profiles:
        with ThreadPoolExecutor(max_workers=4) as pool:
            objects = list(pool.map(compile_object, [(source, defines + (['-DGdBlit=nxr_engine_blit'] if source.stem == 'devblit' else [])) for source in sources]))
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(main_source), '--build', str(build), '--compiler', str(compiler), '-o', str(apps / f'{name}.fxe')]
        for include in includes:
            command += ['-I', str(include)]
        for define in ['MWCONFIG_FILE="nxr_config.h"', 'MWPIXEL_FORMAT=MWPF_TRUECOLOR565', 'NXR_DESKTOP=1', *[d[2:] for d in defines]]:
            command += ['-D', define]
        for obj in objects:
            command += ['--object-ir', str(obj)]
        if name == 'nxeyes':
            command += ['--float-format']
        key = hashlib.sha256(stamp + sdk.digest() + main_source.read_bytes() + str(command).encode())
        for obj in objects:
            key.update(obj.read_bytes())
        # Track included terminal implementation/font and private Nano-X tables.
        for directory in [NX / 'ripple', ROOT / 'ports/graphics', PORT / 'ripple']:
            for file in sorted(directory.glob('*')):
                if file.suffix in ('.c', '.h', '.inc'):
                    key.update(file.read_bytes())
        for file in sorted((ROOT / 'ports/graphics/fonts').glob('*.inc')):
            key.update(file.read_bytes())
        manifest = build / 'nanox-native' / f'{name}.sha256'
        if (apps / f'{name}.fxe').exists() and manifest.exists() and manifest.read_text() == key.hexdigest():
            continue
        subprocess.run(command, check=True)
        manifest.write_text(key.hexdigest())


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
