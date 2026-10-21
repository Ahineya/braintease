#!/usr/bin/env python3
"""Bootstrap QBE as a C16 guest tool emitting x86_64 SysV assembly."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import shutil

from adapt_qbe import generate, HERE, PORTABLE, parallel_moves

ROOT = PORTABLE.parent
sys.path.insert(0, str(PORTABLE))
from guest_build import support, FORMATTERS
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def ensure_host(extended_moves=False):
    """Build an unmodified reference; all generated files stay outside upstream."""
    directory = PORTABLE / ('build/qbe-host-wide-moves' if extended_moves else 'build/qbe-host')
    shutil.copytree(PORTABLE / 'upstream/qbe', directory, dirs_exist_ok=True)
    if extended_moves:
        rega = directory / 'rega.c'
        rega.write_text(parallel_moves(rega.read_text()))
    command = ['make', '-C', str(directory), '-j4', 'CFLAGS=-std=c99 -O2 -Wall -Wextra']
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
        command += ['LDFLAGS=-isysroot ' + sdk]
    subprocess.run(command, check=True)
    return directory / 'qbe'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('-o', type=Path, default=PORTABLE / 'build/apps/qbe.fxe')
    p.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    p.add_argument('--linker', type=Path)
    p.add_argument('--static', action='store_true', help='bundle libc for an independent guest executable')
    p.add_argument('--heap-profile', choices=['hosted', 'bounded'], default='hosted')
    p.add_argument('--host', action='store_true', help='build the unmodified upstream reference')
    p.add_argument('--wide-moves', action='store_true', help='host-only reference with function-sized parallel-move storage')
    p.add_argument('--metrics', action='store_true', help='report tool allocation requests in C16 cells')
    a = p.parse_args()
    if a.host:
        print(ensure_host(a.wide_moves))
        return
    if a.wide_moves:
        p.error('--wide-moves requires --host')
    sources = generate()
    formatter = support(PORTABLE / 'build/guest-support')
    includes = [HERE / 'include', HERE, ROOT / 'ports/fuzix/ripple/include',
                ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include',
                ROOT / 'ports/fuzix/ripple']
    compiler = ROOT / 'target/release/rcc'
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1',
             '-DFUSE_X_USER=1', '-DRIPPLE_FLOAT_FORMAT']
    for directory in includes:
        flags += ['-I', str(directory)]
    for name in FORMATTERS:
        flags += ['-D', name + '=rcc_c_' + name]
    if a.metrics:
        for name in ['malloc','calloc','realloc','free']:
            flags += ['-D', name + '=rcc_c_' + name]
    cache = PORTABLE / 'build/qbe-ir'
    cache.mkdir(exist_ok=True)
    def compile_unit(source):
        digest = hashlib.sha256(str((compiler.stat().st_mtime_ns, flags)).encode())
        for path in c_dependencies(source, includes):
            digest.update(str(path).encode()); digest.update(path.read_bytes())
        output = cache / (source.parent.name + '-' + source.stem + '-' + digest.hexdigest()[:24] + '.ir')
        if not output.exists():
            print('QBE bootstrap: ' + str(source.relative_to(ROOT)), flush=True)
            result = subprocess.run([str(compiler), 'compile', str(source), *flags, '--emit-ir'], capture_output=True)
            if result.returncode:
                raise RuntimeError(str(source) + ':\n' + result.stderr.decode(errors='replace'))
            output.write_bytes(result.stdout)
        return output
    with ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_unit, sources + [PORTABLE / 'src/strtod.c', formatter]))
    support_sources = [HERE / 'guest.c'] + ([PORTABLE / 'guest/metrics.c'] if a.metrics else [])
    command = ['python3', str(ROOT / 'fuse-x/cc.py'), *map(str,support_sources),
               '-I', str(HERE / 'include'), '-I', str(HERE),
               '--float-format', '-DRIPPLE_FLOAT_FORMAT', '--heap-profile', a.heap_profile,
               '--build', str(a.build), '-o', str(a.o)]
    for obj in objects:
        command += ['--object-ir', str(obj)]
    for name in FORMATTERS:
        command += ['-D', name + '=rcc_c_' + name]
    if a.metrics:
        for name in ['malloc','calloc','realloc','free']:
            command += ['-D', name + '=rcc_c_' + name]
    if a.static or a.linker:
        command += ['--static']
    if a.linker:
        command += ['--linker', str(a.linker)]
    a.o.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
