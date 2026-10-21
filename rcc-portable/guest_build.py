#!/usr/bin/env python3
"""Build the C16 frontend for hosted native, freestanding x86 or Ripple guests.

The portable host frontend compiles every compiler source. The existing SDK
still supplies startup/libc and the host backend packages the resulting FXE or RXE.
"""
import argparse
from pathlib import Path
import subprocess
import sys
from adapt import HERE, SOURCES, main as adapt

ROOT = HERE.parent
FORMATTERS = 'printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf'.split()


def support(build):
    """Extend a generated formatter copy, retaining the original license."""
    build.mkdir(parents=True, exist_ok=True)
    path = ROOT / 'ports/fuzix/ripple/libc/format.c'
    source = path.read_text()
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise RuntimeError(f'{path}: guest formatter contract changed: {old}')
        source = source.replace(old, new)
    replace('#include "../float-format.inc"',
            '#include "' + str(path.parent.parent / 'float-format.inc') + '"')
    replace('fputc(c, o->f)', 'putc(c, o->f)')
    replace('unsigned long value = 0;', 'unsigned long long value = 0;')
    replace('long signed_value;', 'long long signed_value;')
    replace("wide = 1;\n            fmt++;\n        } else if (*fmt == 'z')",
            "wide = 1;\n            fmt++;\n            if (*fmt == 'l') {wide = 2;fmt++;}\n        } else if (*fmt == 'j') {\n            wide = 2;fmt++;\n        } else if (*fmt == 'z')")
    replace('wide ? va_arg(ap, long) : va_arg(ap, int)',
            'wide==2 ? va_arg(ap, long long) : wide ? va_arg(ap, long) : va_arg(ap, int)')
    replace('0UL - (unsigned long)signed_value', '0ULL - (unsigned long long)signed_value')
    replace('wide ? va_arg(ap, unsigned long) : va_arg(ap, unsigned)',
            'wide==2 ? va_arg(ap, unsigned long long) : wide ? va_arg(ap, unsigned long) : va_arg(ap, unsigned)')
    output = build / 'format64.c'
    if not output.exists() or output.read_text() != source:
        output.write_text(source)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-o', '--output', type=Path, default=HERE / 'build/apps/rcc-c.fxe')
    parser.add_argument('--os', choices=['fuse-x', 'fuzix', 'x86_64'], default='fuse-x')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--metrics', action='store_true', help='report compiler heap requests and peak live cells')
    parser.add_argument('--map', type=Path, help='write Riplix/RVM RXE symbol locations as JSON')
    args = parser.parse_args()
    if args.map and args.os != 'fuzix':
        parser.error('--map applies to Riplix/RVM RXE builds')
    if args.build is None:
        args.build = ROOT / ('build/guest-native-x64-sdk' if args.os == 'x86_64' else
                             'fuse-x/build' if args.os == 'fuse-x' else 'build/fuzix')
    if args.os == 'fuzix' and args.output == HERE / 'build/apps/rcc-c.fxe':
        args.output = args.output.with_suffix('.rxe')
    if args.os == 'x86_64' and args.output == HERE / 'build/apps/rcc-c.fxe':
        args.output = HERE / 'build/apps/rcc-c-x86.fxe'
    adapt()
    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    formatter = support(HERE / 'build/guest-support')
    sources = [HERE / f'build/frontend/{name}.c' for name in SOURCES]
    sources += sorted((HERE / 'src').glob('*.c'))
    sources += [HERE / 'guest/strtoull.c', formatter]
    if args.metrics:
        sources += [HERE / 'guest/metrics.c']
    command = [sys.executable, str(HERE / ('fuzix.py' if args.os == 'fuzix' else 'fuse_x.py')), *map(str, sources),
               '-I', str(HERE / 'guest/include'), '-I', str(HERE / 'build/frontend'),
               '-DRIPPLE_FLOAT_FORMAT', '--float-format', '--compiler', str(args.compiler),
               '--build', str(args.build), '-o', str(args.output)]
    if args.os == 'x86_64':
        command += ['--heap-profile', 'bounded', '--linker', str(ROOT / 'tools/x86_64/app_link.py')]
    for name in FORMATTERS:
        command += ['-D', f'{name}=rcc_c_{name}']
    if args.map:
        command += ['--map', str(args.map)]
    for name in ['warn', 'error', 'reallocarray', 'progname']:
        command += ['-D', f'{name}=rcc_c_{name}']
    if args.metrics:
        for name in ['malloc', 'calloc', 'realloc', 'free']:
            command += ['-D', f'{name}=rcc_c_{name}']
    subprocess.run(command, check=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'guest compiler build: {error}', file=sys.stderr)
        sys.exit(1)
