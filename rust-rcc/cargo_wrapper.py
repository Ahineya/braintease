#!/usr/bin/env python3
"""Cargo's target rustc wrapper: metadata only, preserving MIR for RCC."""
import os
from pathlib import Path
import subprocess
import sys


def option(args, name):
    for index, value in enumerate(args):
        if value == name:
            return args[index + 1]
        if value.startswith(name + '='):
            return value[len(name) + 1:]
    return None


def codegen(args, name):
    for index, value in enumerate(args):
        value = args[index + 1] if value == '-C' else value[2:] if value.startswith('-C') else ''
        if value.startswith(name + '='):
            return value[len(name) + 1:]
    return ''


def driver_arguments(args, ir, native):
    result = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg in ('--emit', '--out-dir'):
            index += 2
            continue
        if arg.startswith('--emit=') or arg.startswith('--out-dir='):
            index += 1
            continue
        if arg == '-C' and args[index + 1].startswith('incremental='):
            index += 2
            continue
        if arg.startswith('-Cincremental='):
            index += 1
            continue
        result.append(arg)
        index += 1
    result += ['--emit=metadata', '--rcc-output', str(ir)]
    if native:
        result.append('--rcc-native')
    return result


def main():
    rustc, *args = sys.argv[1:]
    target = option(args, '--target')
    if not target or any(value == '--print' or value.startswith('--print=') for value in args):
        return subprocess.call([rustc, *args], close_fds=False)
    emit = option(args, '--emit') or ''
    if 'metadata' not in emit.split(',') or not set(emit.split(',')) <= {'metadata', 'dep-info'}:
        print('rust-rcc: target crates must be checked as metadata; use cargo.py', file=sys.stderr)
        return 1
    kinds = (option(args, '--crate-type') or '').split(',')
    if not kinds or not set(kinds) <= {'bin', 'lib', 'rlib'}:
        print('rust-rcc: target crates must be Rust libraries or an application binary', file=sys.stderr)
        return 1
    if any(value == '-l' or value.startswith('-l') for value in args) or codegen(args, 'link-arg') or codegen(args, 'link-args'):
        print('rust-rcc: native target libraries/link arguments are unsupported; dependencies must supply Rust MIR', file=sys.stderr)
        return 1
    checked = list(args)
    # Cargo check still asks rustc to validate a binary's allocator shim. RCC
    # supplies that shim at its later link stage, so check the root as a library.
    for index, value in enumerate(checked):
        if value == '--crate-type' and checked[index + 1] == 'bin': checked[index + 1] = 'lib'
        elif value == '--crate-type=bin': checked[index] = '--crate-type=lib'
    status = subprocess.call([rustc, *checked], close_fds=False)
    if status:
        return status
    source = Path(os.environ['RUSTCELL_CARGO_SOURCE']).resolve()
    if any(Path(value).resolve() == source for value in args if value.endswith('.rs')):
        name = option(args, '--crate-name') + codegen(args, 'extra-filename')
        directory = Path(os.environ['RUSTCELL_CARGO_RECORDS'])
        directory.mkdir(parents=True, exist_ok=True)
        # Run inside Cargo's exact package/build-script environment. Serializing
        # that environment would miss rustc-env values or save credentials.
        ir = directory / (name + f'.{os.getpid()}.ir.tmp')
        try:
            status = subprocess.call([os.environ['RUSTCELL_CARGO_DRIVER'],
                        *driver_arguments(checked, ir, os.environ['RUSTCELL_CARGO_NATIVE'] == '1')],
                        close_fds=False)
            if status: return status
            ir.replace(directory / (name + '.ir'))
        finally:
            ir.unlink(missing_ok=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
