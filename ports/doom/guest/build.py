#!/usr/bin/env python3
"""Explicit OS guest builds; never install Doom into normal system images."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

DOOM = Path(__file__).resolve().parents[1]
ROOT = DOOM.parents[1]
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def write_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=('fuse-x', 'fuzix'))
    parser.add_argument('--build', type=Path, default=DOOM / 'build/guest')
    parser.add_argument('--sdk-build', type=Path)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--stage', type=Path, help='copy the Fuse-X app and shareware WAD here')
    parser.add_argument('--no-audio', action='store_true', help='build Fuse-X without SFX or OPL3 music')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.stage and args.target != 'fuse-x':
        parser.error('--stage is only supported for Fuse-X')
    build = args.build.resolve() / args.target
    generated = build / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    run(['node', DOOM / 'tools/generate-data.mjs', DOOM / 'engine', generated])
    wad = DOOM / 'assets/doom1.wad'
    write_changed(generated / 'doom_build_config.h',
                  '#ifndef DOOM_BUILD_CONFIG_H\n#define DOOM_BUILD_CONFIG_H\n'
                  '#include "guest.h"\n#define DOOM_WAD_MODE 0\n'
                  '#define DOOM_WAD_PATH doom_guest_wad_path\n'
                  f'#define DOOM_WAD_LENGTH {wad.stat().st_size}UL\n'
                  '#define DOOM_ASSET_STRICT 0\n#endif\n')
    compiler = args.compiler.resolve()
    includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include', ROOT / 'runtime/include',
                PORT / 'ripple', ROOT / 'ports/graphics/include', DOOM / 'guest/include',
                DOOM / 'guest', DOOM / 'src', generated, DOOM / 'engine']
    native = args.target == 'fuse-x'
    audio = int(native and not args.no_audio)
    defines = ['RIPPLE_MMU=1', 'NO_64BIT', 'DOOM_GUEST=1', 'DOOM_PORT=1', f'DOOM_PCM={audio}',
               f'DOOM_MUSIC={audio}', 'DOOM_MOUSE=0', 'DOOM_GENERATED_TABLES=1', 'DOOM_GENERATED_INFO=1',
               'DOOMGENERIC_RESX=320', 'DOOMGENERIC_RESY=200', 'DOOM_RESOLUTION_SCALE=1']
    defines += ['FUSE_X_USER=1'] if native else ['RIPPLE=1']
    flags = ['--bank-size', '64000'] + (['--target', 'native', '--emit-ir'] if native else [])
    flags += [f'-I{p}' for p in includes] + [f'-D{d}' for d in defines]
    names = re.search(r'^ENGINE_NAMES\s*:=\s*(.*?)(?=\n\w)',
                      (DOOM / 'Makefile').read_text(), re.M | re.S)[1].replace('\\\n', ' ').split()
    sources = [DOOM / 'engine' / f'{name}.c' for name in names]
    sources += sorted(generated.glob('tables_*.c')) + sorted(generated.glob('info_*.c'))
    sources += [DOOM / 'src' / f'{name}.c' for name in
                'platform config_port wad_io w_file_port patch_cache alloc_fast asset_metadata'.split()]
    sources += [DOOM / 'guest/platform.c']
    if audio:
        sources += [DOOM / 'src/pcm_sound.c', DOOM / 'guest/audio.c', DOOM / 'guest/music.c']
    if not native:
        sources += [DOOM / 'guest/main.c']
        sources += [DOOM / 'src' / f'{name}_fast.asm' for name in ('fixed', 'render', 'raster', 'memory')]
    objects = build / 'objects'
    objects.mkdir(exist_ok=True)
    tool_identity = str((compiler.stat().st_mtime_ns, flags)).encode()

    def compile_source(source):
        name = ('generated_' if source.parent == generated else source.parent.name + '_') + source.stem
        output = objects / (name + ('.ir' if native else '.pobj'))
        digest = hashlib.sha256(tool_identity)
        for dependency in c_dependencies(source, includes) if source.suffix == '.c' else [source]:
            digest.update(str(dependency).encode())
            digest.update(dependency.read_bytes())
        if not native:
            digest.update(str((ROOT / 'src/ripple-asm/target/release/rasm').stat().st_mtime_ns).encode())
        signature = output.with_suffix('.sha256')
        key = digest.hexdigest()
        if output.exists() and signature.exists() and signature.read_text() == key:
            return output
        print(f'  {args.target}: {source.relative_to(DOOM)}', flush=True)
        if source.suffix == '.c':
            command = [compiler, 'compile', source, *flags]
            if not native:
                assembly = output.with_suffix('.asm')
                command += ['-o', assembly]
            result = subprocess.run(list(map(str, command)), capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{source}:\n{result.stderr.decode(errors="replace")}')
            if native:
                output.write_bytes(result.stdout)
        else:
            assembly = source
        if not native:
            run([ROOT / 'src/ripple-asm/target/release/rasm', 'assemble', assembly, '-o', output,
                 '--bank-size', '64000', '--max-immediate', '65535'])
        signature.write_text(key)
        return output

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        inputs = list(pool.map(compile_source, sources))
    sdk = (args.sdk_build or ROOT / ('fuse-x/build' if native else 'ports/fuzix/build')).resolve()
    output = build / ('doom.fxe' if native else 'doom.rxe')
    if native:
        command = ['python3', ROOT / 'fuse-x/cc.py', DOOM / 'guest/main.c', '--compiler', compiler, '--build', sdk, '-o', output]
        for include in includes[5:]:
            command += ['-I', include]
        for define in defines:
            command += ['-D', define]
        for obj in inputs:
            command += ['--object-ir', obj]
        run(command)
    else:
        softfloat = sdk / 'awk/softfloat.pobj'
        run(['make', '-C', PORT, f'BUILD={sdk}', f'RCC={compiler}', f'-j{args.jobs}', 'rfl-sdk', softfloat])
        result = run(['make', '-s', '-C', PORT, f'BUILD={sdk}', 'rfl-sdk-info'], capture_output=True, text=True)
        info = json.loads(result.stdout)
        run([ROOT / 'src/ripple-asm/target/release/rlink', info['entry'], *inputs, softfloat,
             *info['libraries'], '-f', 'rxe', '--bank-size', '64000', '-o', output])
    if args.stage:
        stage = args.stage.resolve()
        stage.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output, stage / 'doom.fxe')
        shutil.copy2(wad, stage / 'doom1.wad')
        shutil.copy2(DOOM / 'LICENSE', stage / 'doom-LICENSE.txt')
        shutil.copy2(DOOM / 'assets/README.md', stage / 'doom-WAD.txt')
        write_changed(stage / 'doom-README.txt',
                      'Doom for Fuse-X\n\n'
                      'Run /mnt/host/doom.fxe, or add -warp 1 1 to start E1M1.\n'
                      'Keep doom1.wad beside the executable.\n'
                      'Arrows: move/turn; Ctrl: fire; Space: use; Shift: run; A/D: strafe.\n'
                      'Esc opens the menu; closing the window exits.\n'
                      'Resize the window or use its green fullscreen button.\n'
                      + ('OPL3 music and PCM effects enabled; -nosound disables audio.\n' if audio else 'Silent audio build.\n') +
                      'Engine source/build instructions: ports/doom/guest and ports/doom/README.md.\n'
                      'Engine license: doom-LICENSE.txt; separate game data details: doom-WAD.txt.\n')
    print(f'Built {output}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(f'Build command failed (status {error.returncode}); see diagnostics above.')
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
