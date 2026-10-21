#!/usr/bin/env python3
"""Prepare sources and Make rules for four native tools, without host paths.

Guest execution still needs seed rcc-c, QBE and rcc-x64-as, a matching source
SDK, Make and mkdir/rm. This prepares a rebuild; it does not claim one passed.
"""
import argparse
import hashlib
import json
from pathlib import Path
from make_rules import header_rules

HERE = Path(__file__).resolve().parent
PORTABLE = HERE.parent


def tool_sources():
    return (sorted((PORTABLE / 'native').glob('*.c'))
            + [PORTABLE / 'ripple/parse.c', PORTABLE / 'ripple/helpers.c']
            + sorted((PORTABLE / 'x64-link').glob('*.c'))
            + [PORTABLE / 'native-driver/cc.c', PORTABLE / 'native-archive/main.c'])


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def prepare(output, sdk):
    files, rows, groups = {}, [], {}
    sources = tool_sources() + [PORTABLE / 'guest/strtoull.c']
    for source in sources:
        relative = str(source.relative_to(PORTABLE))
        name = 'src/' + relative
        files[name] = source.read_bytes()
        if relative == 'ripple/parse.c':
            # Same declaration shim as the assessed bootstrap, with a relative
            # include so the prepared sources can move to a guest filesystem.
            name = 'src/parser.c'
            files[name] = (b'unsigned long long strtoull(const char *,char **,int);\n'
                           b'#include "ripple/parse.c"\n')
        obj = '$(BUILD)/obj/' + relative.replace('/', '-').removesuffix('.c') + '.o'
        group = ('rcc-qbe' if source.parent.name in ['native', 'ripple', 'guest'] else
                 'rcc-x64-ld' if source.parent.name == 'x64-link' else
                 'rcc-native-cc' if source.parent.name == 'native-driver' else 'rcc-x64-ar')
        groups.setdefault(group, []).append(obj)
        rows.append(dict(source=name, object=obj, tool=group))
    for folder in ['native', 'ripple', 'x64-link']:
        for path in (PORTABLE / folder).glob('*.h'):
            files['src/' + str(path.relative_to(PORTABLE))] = path.read_bytes()
    files['src/native-format.h'] = (PORTABLE / 'native-format.h').read_bytes()
    files['UPSTREAM.md'] = (PORTABLE / 'UPSTREAM.md').read_bytes()
    headers = sorted(name for name in files if name.endswith('.h'))
    # Include the real SDK headers behind its forwarding includes, too. No
    # absolute bootstrap SDK path is written into the portable Makefile.
    sdk_headers = sorted('$(SDK)/' + str(p.relative_to(sdk))
                         for folder in ['include', 'tree']
                         for p in (sdk / folder).rglob('*')
                         if p.is_file() and p.suffix in ['.h', '.inc'])
    seeds = ['$(TOOLS)/' + n for n in ['rcc-c', 'rcc-qbe', 'qbe', 'rcc-x64-as']]
    ready, dependencies, stamps = header_rules(headers + sdk_headers, '$(BUILD)/headers')
    make = ['# Generated native-tool source build; no Python or host compiler in the guest.',
            'BUILD ?= build', 'TOOLS ?= /bin', 'SDK ?= /usr/lib/rcc-native',
            'CC = $(TOOLS)/rcc-native-cc', 'CFLAGS ?=',
            '.PHONY: all clean', '.DELETE_ON_ERROR:',
            'all: ' + ' '.join('$(BUILD)/bin/' + name for name in groups)]
    make += dependencies
    for row in rows:
        make += [row['object'] + ': ' + row['source'] + ' ' + ready + ' src/ripple/parse.c Makefile $(CC) ' + ' '.join(seeds),
                 '\tmkdir -p "$(BUILD)/obj"',
                 '\t"$(CC)" --tools "$(TOOLS)" --sdk "$(SDK)" $(CFLAGS) -c $< -o $@']
    startup = ['$(SDK)/crt0-' + str(n) + '.o' for n in [0, 2, 3]]
    for name, objects in groups.items():
        make += ['$(BUILD)/bin/' + name + ': ' + ' '.join(objects + startup)
                 + ' $(SDK)/libc.a $(TOOLS)/rcc-x64-ld $(CC) Makefile',
                 '\tmkdir -p "$(BUILD)/bin"',
                 '\t"$(CC)" --tools "$(TOOLS)" --sdk "$(SDK)" ' + ' '.join(objects) + ' -o $@']
    make += ['clean:', '\trm -f ' + ' '.join(row['object'] for row in rows)
             + ' ' + ' '.join('$(BUILD)/bin/' + name for name in groups) + ' ' + ' '.join(stamps), '']
    files['Makefile'] = '\n'.join(make).encode()
    files['manifest.json'] = (json.dumps(rows, indent=2) + '\n').encode()
    files['BUILDING.txt'] = b'''Native Riplix tool source build

Run make SDK=/path/to/native-sdk TOOLS=/path/to/seed/bin
Outputs: build/bin/rcc-qbe, rcc-x64-ld, rcc-native-cc, rcc-x64-ar.
BUILD selects a separate output directory; CC selects a driver executable.
Make, mkdir and rm must be installed. No Python is used in the guest build.
The frontend, QBE and assembler are still seed tools, not rebuilt here.
All tools and the SDK must match the freestanding bounded C16 memory profile.
Use make clean before changing CFLAGS or compiler/SDK path selections.
Preserve the SDK's source notices and GPL/LGPL texts when distributing libc.
This source bundle alone does not establish a guest self-rebuild or a stable
second compiler generation. Run the rebuilt tools and compare generations.
'''
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        digest.update(name.encode() + b'\0' + data)
    files['source-id'] = (digest.hexdigest() + '\n').encode()
    for name, data in files.items():
        write(output / name, data)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=PORTABLE.parent / 'build/portable-native-sdk')
    parser.add_argument('--output', type=Path, default=PORTABLE.parent / 'build/portable-native-tool-source')
    args = parser.parse_args()
    rows = prepare(args.output.resolve(), args.sdk.resolve())
    print(f'{len(rows)} source units prepared in {args.output.resolve()}')


if __name__ == '__main__':
    main()
