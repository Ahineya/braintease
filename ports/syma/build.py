#!/usr/bin/env python3
"""Build the shared Syma C16 runtime without opening installed guest disks."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
SOURCES = ['term', 'parse', 'engine', 'primitives', 'module', 'effects', 'repl', 'session', 'worksheet', 'main']
SUPPORT = ['libregexp', 'libunicode', 'cutils', 'dtoa']


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def host(build, sanitize):
    support = ROOT / 'ports/quickjs/upstream'
    sdk = (['-isysroot', subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()]
        if sys.platform == 'darwin' else [])
    cc = os.environ.get('CC', '/usr/bin/clang' if sys.platform == 'darwin' else 'cc')
    flags = ['-std=c99', '-D_GNU_SOURCE', '-O1' if sanitize else '-O2', '-g']
    if sanitize:
        flags += ['-fsanitize='+sanitize, '-fno-omit-frame-pointer']
    destination = build / ('syma-sanitize' if sanitize else 'syma')
    temporary = destination.with_suffix('.new')
    inputs = [PORT / (name + '.c') for name in SOURCES]
    inputs += [support / (name + '.c') for name in SUPPORT]
    core_count = len(inputs)
    compat = build / 'curses-include'
    compat.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'ports/fuzix/ripple/include/curses.h', compat / 'curses.h')
    shutil.copy2(ROOT / 'runtime/include/ripple_unicode.h', compat / 'ripple_unicode.h')
    termcap = (ROOT / 'ports/fuzix/upstream/Library/include/termcap.h').read_text()
    (compat / 'termcap.h').write_text(termcap.replace('#include <types.h>', '#include <sys/types.h>'))
    curses = ROOT / 'ports/fuzix/upstream/Library/libs/curses'
    overrides = {'initscr', 'endwin', 'newwin', 'windel', 'wgetch', 'wgetstr',
                 'doupdt', 'refresh', 'winscrol', 'waddch', 'waddstr'}
    inputs += sorted(p for p in curses.glob('*.c') if p.stem not in overrides)
    inputs += sorted((ROOT / 'ports/fuzix/ripple/curses').glob('*.c'))
    inputs += [ROOT / 'ports/fuzix/upstream/Library/libs/termcap.c',
               ROOT / 'ports/fuzix/upstream/Library/libs/tgetent.c',
               ROOT / 'libraries/text/text.c', ROOT / 'libraries/unicode/unicode.c']
    flags += ['-DRIPPLE_CURSES_HOST=1', '-I' + str(compat), '-I' + str(curses)]
    objects = []
    object_dir = build / ('objects-sanitize' if sanitize else 'objects')
    object_dir.mkdir(exist_ok=True)
    for source in inputs:
        obj = object_dir / (source.stem + '.o')
        run([cc, *sdk, *flags, '-I' + str(support), '-c', source, '-o', obj])
        objects.append(obj)
    run([cc, *sdk, *flags, *objects, '-lm', '-o', temporary])
    archive = build / ('libsyma-sanitize.a' if sanitize else 'libsyma.a')
    staged_archive = archive.with_suffix('.new.a')
    staged_archive.unlink(missing_ok=True)
    run(['ar', 'rcs', staged_archive,
         *[obj for obj in objects[:core_count] if obj.stem not in ('main', 'worksheet')]])
    staged_archive.replace(archive)
    temporary.replace(destination)
    return destination


def riplix(build, support, inputs, jobs):
    sys.path.insert(0, str(ROOT / 'scripts'))
    from build_cache import c_dependencies
    sdk = ROOT / 'build/fuzix'
    make = ['make', '-s', '--no-print-directory', '-C', ROOT / 'ports/fuzix',
            'BUILD=' + str(sdk), 'RFL_CURSES=1']
    run([*make, '-j' + str(jobs), 'rfl-sdk', 'float-sdk'])
    info = json.loads(subprocess.check_output(list(map(str, [*make, 'rfl-sdk-info'])), text=True))
    floats = json.loads(subprocess.check_output(list(map(str, [*make, 'float-sdk-info'])), text=True))
    libraries = [p for p in info['libraries'] if not p.endswith('/libc/format.pobj')]
    libraries += floats['libraries']
    includes = [PORT, support, ROOT / 'ports/fuzix/ripple/include',
                ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include',
                ROOT / 'ports/fuzix/ripple']
    compiler = ROOT / 'target/release/rcc'
    assembler = ROOT / 'src/ripple-asm/target/release/rasm'
    identity = repr((compiler.stat().st_mtime_ns, assembler.stat().st_mtime_ns,
                     'banked-globals', includes)).encode()

    def compile_one(source):
        ir = build / (source.stem + '.ir')
        assembly, obj, stamp = ir.with_suffix('.asm'), ir.with_suffix('.pobj'), ir.with_suffix('.sha256')
        digest = hashlib.sha256(identity)
        for dependency in c_dependencies(source, includes):
            digest.update(str(dependency).encode())
            digest.update(dependency.read_bytes())
        key = digest.hexdigest()
        if obj.exists() and stamp.exists() and stamp.read_text() == key:
            return obj
        print('riplix: ' + source.name, flush=True)
        with ir.open('w') as output, ir.with_suffix('.log').open('w') as log:
            run([compiler, 'compile', source, '--target', 'ripple', '--bank-size', '64000',
                 '-DRIPPLE_MMU=1', '-D_BSD_SOURCE=1',
                 *[v for directory in includes for v in ['-I', directory]], '--emit-ir'],
                stdout=output, stderr=log)
            run([compiler, 'generate-asm', '-i', ir, '--banked-globals', '-o', assembly],
                stdout=log, stderr=log)
            run([assembler, 'assemble', assembly, '-o', obj, '--bank-size', '64000',
                 '--max-immediate', '65535'], stdout=log, stderr=log)
        stamp.write_text(key)
        return obj

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(compile_one, inputs))
    destination = build / 'syma.rxe'
    run([assembler.with_name('rlink'), info['entry'], *objects, *libraries,
         '-f', 'rxe', '--bank-size', '64000', '-o', destination])
    return destination


def stage(binary, destination):
    destination.mkdir(parents=True, exist_ok=True)
    staged = destination / '.syma.fxe.new'
    shutil.copy2(binary, staged)
    staged.replace(destination / 'syma.fxe')
    assets = destination / 'syma'
    assets.mkdir(exist_ok=True)
    for name in ('library', 'examples'):
        shutil.copytree(PORT / name, assets / name, dirs_exist_ok=True)
    for name in ('README.md', 'WORKSHEET.md', 'session.h', 'PROVENANCE.txt', 'LICENSE.syma.txt', 'LICENSE.quickjs.txt', 'LICENSE.curses-GPL-2.txt', 'LICENSE.curses-LGPL-2.1.txt'):
        shutil.copy2(PORT / name, assets / name)
    print('Staged ' + str(destination / 'syma.fxe'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['host', 'fuse-x', 'fuzix'])
    parser.add_argument('--sanitize', nargs='?', const='undefined', choices=['undefined','address,undefined'], help='host sanitizer instrumentation')
    parser.add_argument('--stage', type=Path, help='stage the Fuse-X executable and original library/examples')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.stage and args.target != 'fuse-x':
        parser.error('--stage requires fuse-x')
    if args.sanitize and args.target != 'host':
        parser.error('--sanitize requires host')
    build = PORT / 'build' / args.target
    build.mkdir(parents=True, exist_ok=True)
    if args.target == 'host':
        binary = host(build, args.sanitize)
    else:
        sys.path.insert(0, str(ROOT / 'ports/quickjs'))
        from adapt import prepare
        support = prepare(build / 'support')
        inputs = [PORT / (name + '.c') for name in SOURCES]
        inputs += [support / (name + '.c') for name in SUPPORT]
        inputs += [ROOT / 'libraries/text/text.c']
        if args.target == 'fuse-x':
            binary = build / 'syma.fxe'
            run([sys.executable, ROOT / 'fuse-x/cc.py', *inputs, '-I', PORT, '-I', support,
                 '-D', '_BSD_SOURCE=1', '--float-format', '--curses', '--static', '-o', binary])
        else:
            binary = riplix(build, support, inputs, args.jobs)
    if args.stage:
        stage(binary, args.stage.resolve())
    print('Built ' + str(binary))


if __name__ == '__main__':
    main()
