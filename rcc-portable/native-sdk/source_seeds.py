#!/usr/bin/env python3
"""Prepare movable sources for the remaining native bootstrap tools.

Preparation is not a source self-build claim. Real x86 guest Make and execution
checks separately validate the resulting tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
PORTABLE = HERE.parent
sys.path.insert(0, str(PORTABLE))
from adapt import SOURCES, main as adapt_frontend
from guest_build import FORMATTERS, support
from make_rules import header_rules

TOOLS = ['rcc-x64-as', 'rcc-c', 'qbe']


def prepare(output, sdk, tool):
    files, units = {}, []
    flags = []
    if tool == 'rcc-x64-as':
        units = [*sorted((PORTABLE / 'x64').glob('*.c')), PORTABLE / 'guest/strtoull.c']
        files['src/x64/x64.h'] = (PORTABLE / 'x64/x64.h').read_bytes()
    else:
        flags = ['-Isrc/include', '-DRIPPLE_FLOAT_FORMAT']
        flags += ['-D' + n + '=rcc_c_' + n for n in FORMATTERS]
        # The private C16 formatter retains 64-bit diagnostics, without changing
        # the SDK's public formatter. Keep its original notices and include.
        formatter = support(PORTABLE / 'build/guest-support').read_text()
        formatter = formatter.replace(str(PORTABLE.parent / 'ports/fuzix/ripple/float-format.inc'),
                                      'float-format.inc')
        files['src/support/format64.c'] = formatter.encode()
        # The accepted minimal SDK predates decimal strtod support. Compile
        # its existing libc implementation as a source unit of each tool;
        # keep the SDK snapshot and the compiler's hexadecimal wrapper intact.
        decimal = (PORTABLE.parent / 'ports/fuzix/upstream/Library/libs/strtod.c').read_text()
        decimal = decimal.replace('#include <stdlib.h>', '#include <stdlib.h>\n#undef strtod')
        files['src/support/decimal-strtod.c'] = decimal.encode()
        files['src/support/math-bits.c'] = (PORTABLE.parent / 'ports/fuzix/ripple/libc/math-bits.c').read_bytes()
        files['src/support/float-format.inc'] = (PORTABLE.parent / 'ports/fuzix/ripple/float-format.inc').read_bytes()
        files['licenses/fuzix-library/COPYING'] = (PORTABLE.parent / 'ports/fuzix/upstream/Library/COPYING').read_bytes()
        files['src/include/sdk-stdio.h'] = (sdk / 'tree/ports/fuzix/ripple/include/stdio.h').read_bytes()
        files['src/include/stdio.h'] = (PORTABLE / 'guest/include/stdio.h').read_bytes().replace(
            b'../../../ports/fuzix/ripple/include/stdio.h', b'sdk-stdio.h')
        files['src/include/sdk-stdlib.h'] = (sdk / 'tree/ports/fuzix/upstream/Library/include/stdlib.h').read_bytes()
        files['src/include/stdlib.h'] = b'''#include "sdk-stdlib.h"
#define strtoull rcc_c_strtoull
unsigned long long strtoull(const char *, char **, int);
#define strtod rcc_c_strtod
double strtod(const char *, char **);
'''
        if tool == 'rcc-c':
            adapt_frontend()
            flags += ['-Isrc/frontend']
            flags += ['-D' + n + '=rcc_c_' + n for n in ['warn', 'error', 'reallocarray', 'progname']]
            units = [PORTABLE / ('build/frontend/' + n + '.c') for n in SOURCES]
            units += sorted((PORTABLE / 'src').glob('*.c')) + [PORTABLE / 'guest/strtoull.c']
            for p in (PORTABLE / 'build/frontend').glob('*.h'):
                files['src/frontend/' + p.name] = p.read_bytes()
            for p in (PORTABLE / 'upstream/cproc').glob('LICENSE*'):
                files['licenses/cproc/' + p.name] = p.read_bytes()
        else:
            sys.path.insert(0, str(PORTABLE / 'qbe'))
            from adapt_qbe import generate, OUTPUT
            units = generate() + [PORTABLE / 'qbe/guest.c', PORTABLE / 'src/strtod.c']
            flags += ['-Isrc/qbe-c16', '-Isrc/qbe', '-Isrc/qbe/include']
            for p in OUTPUT.rglob('*.h'):
                files['src/qbe-c16/' + str(p.relative_to(OUTPUT))] = p.read_bytes()
            for p in (PORTABLE / 'qbe').glob('*.h'):
                files['src/qbe/' + p.name] = p.read_bytes()
            files['src/qbe/arena.inc'] = (PORTABLE / 'qbe/arena.inc').read_bytes()
            for p in (PORTABLE / 'qbe/include').glob('*.h'):
                if p.name != 'stdio.h':
                    files['src/qbe/include/' + p.name] = p.read_bytes()
            for p in (PORTABLE / 'upstream/qbe').glob('LICENSE*'):
                files['licenses/qbe/' + p.name] = p.read_bytes()
    rows = []
    for p in units:
        relative = str(p.relative_to(PORTABLE)).removeprefix('build/')
        source = 'src/' + relative
        files[source] = p.read_bytes()
        rows.append(dict(source=source, object='build/obj/' + relative.replace('/', '-')[:-2] + '.o', tool=tool))
    if tool != 'rcc-x64-as':
        rows.append(dict(source='src/support/format64.c', object='build/obj/support-format64.o', tool=tool))
        rows.append(dict(source='src/support/decimal-strtod.c', object='build/obj/support-decimal-strtod.o', tool=tool))
        rows.append(dict(source='src/support/math-bits.c', object='build/obj/support-math-bits.o', tool=tool))
    files['smoke.c'] = b'int seed_source_probe(void) { return 37; }\n'
    headers = sorted(n for n in files if n.endswith(('.h', '.inc')))
    headers += sorted('$(SDK)/' + str(p.relative_to(sdk)) for folder in ['include', 'tree']
                      for p in (sdk / folder).rglob('*') if p.suffix in ('.h', '.inc'))
    ready, deps, stamps = header_rules(headers, 'build/headers')
    make = ['TOOLS ?= /bin', 'SDK ?= /usr/lib/rcc-native', 'CC = $(TOOLS)/rcc-native-cc',
            'CFLAGS ?=', '.PHONY: all clean', '.DELETE_ON_ERROR:', 'all: build/bin/' + tool]
    make += deps
    seeds = ' '.join('$(TOOLS)/' + n for n in ['rcc-c', 'rcc-qbe', 'qbe', 'rcc-x64-as'])
    for row in rows:
        make += [row['object'] + ': ' + row['source'] + ' ' + ready + ' Makefile $(CC) ' + seeds,
                 '\tmkdir -p build/obj', '\t$(CC) --tools $(TOOLS) --sdk $(SDK) $(CFLAGS) '
                 + ' '.join(flags) + ' -c $< -o $@']
    objects = ' '.join(row['object'] for row in rows)
    make += ['build/bin/' + tool + ': ' + objects + ' $(SDK)/libc.a ' +
             ' '.join('$(SDK)/crt0-' + str(n) + '.o' for n in [0, 2, 3]) + ' Makefile $(CC) $(TOOLS)/rcc-x64-ld',
             '\tmkdir -p build/bin', '\t$(CC) --tools $(TOOLS) --sdk $(SDK) ' + objects + ' -o $@',
             'clean:', '\trm -f ' + objects + ' build/bin/' + tool + ' ' + ' '.join(stamps), '']
    files['Makefile'] = '\n'.join(make).encode()
    files['manifest.json'] = (json.dumps(rows, indent=2) + '\n').encode()
    files['UPSTREAM.md'] = (PORTABLE / 'UPSTREAM.md').read_bytes()
    identity = hashlib.sha256()
    for name, data in sorted(files.items()):
        identity.update(name.encode() + b'\0' + data)
    files['source-id'] = (identity.hexdigest() + '\n').encode()
    for name, data in files.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    return rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', choices=TOOLS, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(str(len(prepare(args.output.resolve(), args.sdk.resolve(), args.tool))) + ' source units prepared')
