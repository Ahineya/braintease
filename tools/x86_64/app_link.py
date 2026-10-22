#!/usr/bin/env python3
"""Fuse-X cc.py linker adapter for the freestanding RXE1 x86_64 ABI.

The compiler's ordinary static guest-libc application ABI is retained. Only
the host shared-object container is replaced: explicit helper slots, flattened
ELF load segments, checked RELATIVE relocations and bounded binary metadata.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from boot import ROOT, executable, elf_linker

TAG = 0x31455852  # RXE1
IMAGE_MAX = 32 * 1024 * 1024  # Entire x64 RXE1 package; matches boot/images.inc.


def flatten(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', data, 16) != (3, 62):
        raise RuntimeError('application linker must produce x86_64 ELF PIE')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phcount)]
    if any(p[0] == 3 for p in segments):
        raise RuntimeError('freestanding application cannot have an interpreter')
    loads = [p for p in segments if p[0] == 1]
    size = max(p[3] + p[6] for p in loads)
    if not 0 < size <= IMAGE_MAX:
        raise RuntimeError('application exceeds 32 MiB load span')
    flat = bytearray(size)
    for _, _, offset, address, _, files, memory, _ in loads:
        if files > memory or offset + files > len(data):
            raise RuntimeError('invalid ELF load span')
        flat[address:address + files] = data[offset:offset + files]
    shoff = struct.unpack_from('<Q', data, 40)[0]
    shsize, shcount = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shsize) for i in range(shcount)]
    symbols, relocations = {}, []
    for section in sections:
        if section[1] == 2:
            strings = sections[section[6]]
            names = data[strings[4]:strings[4] + strings[5]]
            for at in range(section[4], section[4] + section[5], section[9]):
                name, info, _, index, value, _ = struct.unpack_from('<IBBHQQ', data, at)
                if not name or info >> 4 not in (1, 2):
                    continue
                symbol = names[name:names.index(0, name)].decode()
                if not index:
                    raise RuntimeError(f'unresolved freestanding application symbol: {symbol}')
                symbols[symbol] = value
        if section[1] == 4 and section[2] & 2:
            for at in range(section[4], section[4] + section[5], 24):
                address, info, addend = struct.unpack_from('<QQq', data, at)
                if info != 8 or address + 8 > size or not 0 <= addend < size:
                    raise RuntimeError('application needs unsupported ELF relocation')
                relocations.append((address, addend))
    return bytes(flat), symbols, relocations


def pack_image(meta, code, symbols, relocations):
    helpers = meta['helpers']
    # The loader page-aligns the package, but RXE1's tables are only 8-aligned.
    # Preserve ELF's page load bias inside the code blob. All native offsets,
    # including relative pointer addends, must move together; C16 initializers
    # and function tokens remain unchanged.
    metadata = 64 + len(meta['functions'])*16 + len(helpers)*72 + len(relocations)*16 + len(meta['initializers'])*24
    padding = (-metadata) % 4096
    # The runtime streams one bounded package allocation, not just its code.
    # Account for every table and page-load-bias byte before constructing it.
    if metadata + padding + len(code) > IMAGE_MAX:
        raise RuntimeError('application exceeds 32 MiB package including metadata and alignment')
    code = bytes(padding) + code
    symbols = {name: value + padding for name, value in symbols.items()}
    relocations = [(offset + padding, addend + padding) for offset, addend in relocations]
    functions = b''.join(struct.pack('<IIII', f['token'] & 0x7fffffff, f['arity'],
                                    symbols[f['symbol']], 0) for f in meta['functions'])
    helper_table = b''
    for helper in helpers:
        name = helper['symbol'].encode()
        if len(name) >= 64:
            raise RuntimeError('helper name exceeds RXE1 capacity')
        helper_table += struct.pack('<64sII', name, helper['arity'], 0)
    relocs = b''.join(struct.pack('<QQ', *r) for r in relocations)
    initializers = b''.join(struct.pack('<IIQII', p, width, value, int(function), 0)
                            for p, value, width, function in meta['initializers'])
    body = functions + helper_table + relocs + initializers + code
    header = struct.pack('<8s14I', b'FUSEX001', TAG, 64 + len(body), len(code),
                         len(meta['functions']), len(helpers), len(relocations), len(meta['initializers']),
                         meta['entry'] & 0x7fffffff, meta['global_bank'], meta['global_cells'],
                         meta['errno'], symbols['riplix_helpers'], 0, 0)
    return header + body, padding


def link(inputs, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.riplix-link-', dir=output.parent) as temporary:
        directory = Path(temporary)
        obj, manifest = directory / 'app.o', directory / 'manifest.json'
        subprocess.run([os.environ.get('RIPLIX_NATIVE_OBJECT', str(ROOT / 'target/release/rcc-native-object')), '--triple', 'x86_64-unknown-none',
                        '--application', str(manifest), *map(str, inputs), '-o', str(obj)], check=True)
        meta = json.loads(manifest.read_text())
        if (meta['arch'], meta['os'], meta['abi']) != ('x86_64', 'none', 2) or meta.get('module'):
            raise RuntimeError('expected static freestanding application manifest')
        helpers = meta['helpers']
        glue = ['typedef unsigned long long U;', f'U riplix_helpers[{max(1, len(helpers))}];']
        for index, helper in enumerate(helpers):
            arity = helper['arity']
            parameters = ['U ctx', *[f'U a{i}' for i in range(arity)]]
            values = ['ctx', *[f'a{i}' for i in range(arity)]]
            glue.append(f'U {helper["symbol"]}({",".join(parameters)}) {{ return '
                        f'((U (*)({",".join(["U"] * (arity + 1))}))riplix_helpers[{index}])'
                        f'({",".join(values)}); }}')
        source, bindings = directory / 'bindings.c', directory / 'bindings.o'
        source.write_text('\n'.join(glue) + '\n')
        subprocess.run([executable('clang'), '--target=x86_64-unknown-none-elf', '-ffreestanding',
                        '-fno-builtin', '-fno-stack-protector', '-mno-red-zone', '-fPIC', '-O2',
                        '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-c', str(source),
                        '-o', str(bindings)], check=True)
        elf = directory / 'app.elf'
        subprocess.run([*elf_linker(), '-pie', '--no-dynamic-linker', '-Bsymbolic', '--no-undefined',
                        '-e', 'rcc_user_main', '-o', str(elf), str(obj), str(bindings)], check=True)
        code, symbols, relocations = flatten(elf)
        package_bytes, padding = pack_image(meta, code, symbols, relocations)
        package = directory / 'application.fxe'
        package.write_bytes(package_bytes)
        package.replace(output)
        output.with_suffix('.json').write_text(json.dumps(dict(format='RXE1', architecture='x86_64',
            functions=len(meta['functions']), code_bytes=len(code), package_bytes=len(package_bytes), code_padding_bytes=padding,
            helpers=helpers, global_cells=meta['global_cells']), indent=2) + '\n')
        print(f'Built freestanding application: {output} ({len(package_bytes):,} bytes)', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['link-app'])
    parser.add_argument('inputs', type=Path, nargs='+')
    parser.add_argument('-o', type=Path, required=True)
    args = parser.parse_args()
    link(args.inputs, args.o)
