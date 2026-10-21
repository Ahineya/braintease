#!/usr/bin/env python3
"""Compile NetSurf's local-page browser core, with optional web features off."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from adapt import prepare, write_changed
from build import ROOT, PORT, includes


def core_includes(source):
    return [PORT / 'config', source / 'netsurf', source / 'netsurf/include',
            source / 'netsurf/content/handlers', source / 'libnsutils/include',
            *includes(source)]


def core_sources(source):
    names = subprocess.check_output(['make', '-s', '--no-print-directory', '-f',
        str(PORT / 'config/sources.mk'), 'sources'], cwd=source / 'netsurf', text=True).split()
    return [source / 'netsurf' / name for name in sorted(set(names))] + [
        source / 'libnsutils/src/base64.c', source / 'libnsutils/src/time.c',
        PORT / 'numeric.c', PORT / 'cache/store.c']


def build_core(output, compiler, target, jobs=3):
    output, compiler = Path(output).resolve(), Path(compiler).resolve()
    source = prepare(output / 'source')
    objects = output / ('core-' + target)
    objects.mkdir(parents=True, exist_ok=True)
    # The core uses the full floating-point SDK. NO_64BIT also remaps
    # double math calls to the SDK's absent float variants, so omit it here.
    flags = ['--target', target, '-DRIPPLE_MMU=1', '-DNDEBUG']
    if target == 'native':
        flags += ['-DRIPPLE_NETSURF_HOSTED=1']
    flags += [v for directory in core_includes(source) for v in ('-I', str(directory))]
    digest = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for directory in [PORT / 'config', source]:
        for path in sorted(directory.rglob('*')):
            if path.is_file() and path.suffix in ('.h', '.inc'):
                digest.update(str(path.relative_to(directory)).encode()); digest.update(path.read_bytes())
    base = digest.digest()
    tasks = core_sources(source)
    if target == 'ripple':
        # Fuse-X's SDK already supplies Unicode; the minimal RVM SDK does not.
        tasks.append(ROOT / 'libraries/unicode/unicode.c')
        for path in (ROOT / 'runtime/include/ripple_unicode.h', ROOT / 'libraries/unicode/tables.inc'):
            digest.update(path.read_bytes())
        base = digest.digest()

    def compile_one(path):
        name = '_'.join(path.relative_to(source).with_suffix('').parts) if path.is_relative_to(source) else path.stem
        ir, stamp = objects / (name + '.ir'), objects / (name + '.json')
        key = hashlib.sha256(base + path.read_bytes()).hexdigest()
        try:
            cached = ir.exists() and ir.stat().st_size and json.loads(stamp.read_text())['key'] == key
        except (OSError, ValueError, KeyError):
            cached = False
        if not cached:
            result = subprocess.run([str(compiler), 'compile', str(path), *flags, '--emit-ir'],
                                    capture_output=True, env=dict(os.environ, RUST_LOG='error'))
            ir.with_suffix('.log').write_bytes(result.stderr + (result.stdout if result.returncode else b''))
            if result.returncode:
                return None, f'{path}: see {ir.with_suffix(".log")}'
            write_changed(ir, result.stdout)
            stamp.write_text(json.dumps(dict(key=key)) + '\n')
        return ir, None

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(compile_one, tasks))
    failures = [failure for _, failure in results if failure]
    paths = [path for path, failure in results if not failure]
    if failures:
        raise RuntimeError('\n'.join(failures))
    write_changed(objects / 'objects.json', (json.dumps(list(map(str, paths)), indent=2) + '\n').encode())
    print(f'Compiled {len(paths)} NetSurf core objects ({target})', flush=True)
    return paths


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/netsurf')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('-j', '--jobs', type=int, default=3)
    args = parser.parse_args()
    try:
        build_core(args.build, args.compiler, args.target, args.jobs)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr); sys.exit(1)
