#!/usr/bin/env python3
"""Prepare the initial source snapshot; guest builds need only make, Tcl and cc."""
import argparse
import hashlib
from pathlib import Path
from adapt import HERE, main as adapt
from guest_build import FORMATTERS, support

ROOT = HERE.parent


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def prepare(destination=None):
    destination = destination or HERE / 'build/source'
    adapt()
    files = {}
    for path in (HERE / 'build/frontend').glob('*.[ch]'):
        files['frontend/' + path.name] = path.read_bytes()
    for path in (HERE / 'src').glob('*.c'):
        files['frontend/src-' + path.name] = path.read_bytes()
    for folder in ['asm', 'ripple']:
        for path in (HERE / folder).glob('*.[ch]'):
            files[folder + '/' + path.name] = path.read_bytes()
    files['driver/cc.c'] = (HERE / 'driver/cc.c').read_bytes()
    files['support/strtoull.c'] = (HERE / 'guest/strtoull.c').read_bytes()
    formatter = support(HERE / 'build/guest-support').read_text()
    formatter = formatter.replace(str(ROOT / 'ports/fuzix/ripple/float-format.inc'), 'float-format.inc')
    files['support/format64.c'] = formatter.encode()
    files['support/float-format.inc'] = (ROOT / 'ports/fuzix/ripple/float-format.inc').read_bytes()
    files['guest/include/sdk-stdlib.h'] = (ROOT / 'ports/fuzix/upstream/Library/include/stdlib.h').read_bytes()
    files['guest/include/stdlib.h'] = b'''#include "sdk-stdlib.h"
#define strtoull rcc_c_strtoull
unsigned long long strtoull(const char *, char **, int);
#define strtod rcc_c_strtod
double strtod(const char *, char **);
'''
    files['guest/include/sdk-stdio.h'] = (ROOT / 'ports/fuzix/ripple/include/stdio.h').read_bytes()
    files['guest/include/stdio.h'] = (HERE / 'guest/include/stdio.h').read_bytes().replace(
        b'../../../ports/fuzix/ripple/include/stdio.h', b'sdk-stdio.h')
    files['bootstrap.tcl'] = (HERE / 'bootstrap.tcl').read_bytes()
    for name in ['toolchain', 'sdk_null']:
        files[f'tests/{name}.c'] = (HERE / f'tests/{name}.c').read_bytes()
    for path in (HERE / 'upstream/cproc').glob('LICENSE*'):
        files[path.name] = path.read_bytes()
    files['UPSTREAM.md'] = (HERE / 'UPSTREAM.md').read_bytes()

    definitions = [f'-D{name}=rcc_c_{name}' for name in [*FORMATTERS, 'warn', 'error', 'reallocarray', 'progname']]
    make = ['# Generated initial source snapshot; builds use only guest tools.',
            'BUILD ?= build', 'CC ?= cc', 'TOOLS ?= /bin', 'SDK ?= /usr/lib/rcc',
            'CFLAGS ?=', 'CFLAGS += -Ifrontend -Iguest/include -DRIPPLE_MMU=1 -DRIPPLE_FLOAT_FORMAT ' + ' '.join(definitions),
            '.PHONY: all tools', '.DELETE_ON_ERROR:',
            'all: $(BUILD)/bin/rcc-c $(BUILD)/bin/rcc-ripple $(BUILD)/bin/rcc-as $(BUILD)/bin/rcc-ld $(BUILD)/bin/cc',
            'tools: $(BUILD)/bin/rcc-as $(BUILD)/bin/rcc-ld $(BUILD)/bin/cc']
    headers = sorted(name for name in files if name.endswith(('.h', '.inc')))
    groups = {}
    for name in sorted(files):
        if not name.endswith('.c') or name.startswith('tests/'):
            continue
        group = name.split('/')[0]
        obj = '$(BUILD)/obj/' + name.replace('/', '-').removesuffix('.c') + '.rpo'
        groups.setdefault(group, []).append(obj)
        make += [f'{obj}: {name} ' + ' '.join(headers) + ' Makefile $(BUILD)/inputs.stamp $(SDK)/sdk-id $(TOOLS)/rcc-c $(TOOLS)/rcc-ripple $(TOOLS)/rcc-as',
                 '\tmkdir -p $(BUILD)/obj $(BUILD)/bin',
                 '\t$(CC) --tools $(TOOLS) --sdk $(SDK) $(CFLAGS) -c $< -o $@']
    for name, group in [('rcc-c', 'frontend'), ('rcc-ripple', 'ripple'), ('rcc-as', 'asm'), ('cc', 'driver')]:
        make += [f'$(BUILD)/bin/{name}: ' + ' '.join(groups[group] + groups['support']) + ' $(SDK)/libc.rpa $(SDK)/entry.rpo',
                 '\t$(CC) --tools $(TOOLS) --sdk $(SDK) ' + ' '.join(groups[group] + groups['support']) + ' -o $@']
    make += ['$(BUILD)/bin/rcc-ld: $(BUILD)/bin/rcc-as', '\tcp $< $@', '']
    files['Makefile'] = '\n'.join(make).encode()
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        digest.update(name.encode() + b'\0' + data)
    files['source-id'] = (digest.hexdigest() + '\n').encode()
    for relative, data in files.items():
        write(destination / relative, data)
    for path in destination.rglob('*'):
        if path.is_file() and str(path.relative_to(destination)) not in files:
            path.unlink()
    return destination


def sdk_identity(sdk, output=None):
    digest = hashlib.sha256()
    for path in sorted(sdk.rglob('*')):
        if path.is_file() and path.relative_to(sdk) != Path('sdk-id'):
            digest.update(str(path.relative_to(sdk)).encode() + b'\0')
            digest.update(path.read_bytes())
    path = output or HERE / 'build/sdk-id'
    write(path, (digest.hexdigest() + '\n').encode())
    return path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    print(prepare(args.output))
