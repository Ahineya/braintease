#!/usr/bin/env python3
"""Compare unmodified registry crates under native Rust, Fuse-X and Riplix/RVM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib

import compile as compiler
from test import Terminal, check_image

ROOT, HERE = compiler.ROOT, compiler.HERE
PROJECT = HERE / 'examples/crates/Cargo.toml'
CRATES = ['byteorder', 'heapless', 'dynamic', 'postcard', 'postcard-derived']


def execute(command, log, environment=None):
    with log.open('wb') as output:
        result = subprocess.run(list(map(str, command)), env=environment,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'failed; see {log}\n' + log.read_text(errors='replace')[-7000:])



def guest(system, name, app, expected, args):
    build = args.fuzix_build.resolve() if system == 'fuzix' else args.fuse_x_build.resolve()
    output = args.build.resolve() / 'crates'
    with tempfile.TemporaryDirectory(prefix='crate-' + system + '-') as temporary:
        image = Path(temporary) / 'test.img'
        compiler.run('node', HERE / 'tests/image.mjs', system, build, app, image)
        check_image(system, image)
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                           RCC_NATIVE_TIMEOUT='180', RIPPLE_NET='off')
        if system == 'fuzix':
            command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                       '--memory', '16384000', '--jit', '--net', 'off', '--audio', 'off', '--timeout', '180']
            prompt, shutdown = b'riplix# ', b'Halted.'
        else:
            command = [build / 'fuse-x', '--run', image]
            prompt, shutdown = b'fuse-x# ', b'filesystem synced; session stopped'
        terminal = Terminal(command, environment)
        try:
            terminal.until(prompt)
            command = (b'rustprobe;echo CRATE_STATUS:$?;rustprobe panic;echo UNWRAP_STATUS:$?;'
                       b'rustprobe formatted;echo FORMAT_STATUS:$?;')
            if name == 'dynamic':
                command += b'rustprobe callback;echo CALLBACK_STATUS:$?;'
            if name == 'byteorder':
                command += b'rustprobe short;echo SHORT_STATUS:$?;'
            terminal.send(command + b'shutdown\r')
            terminal.until(b'CRATES_OK\n', unexpected=prompt)
            terminal.until(b'CRATE_STATUS:0\n')
            terminal.until(b'UNWRAP_STATUS:101\n')
            terminal.until(b'PANIC_ARGUMENT\n')
            terminal.until(b'FORMAT_STATUS:101\n')
            if name == 'dynamic':
                terminal.until(b'DYNAMIC_WAITING', unexpected=prompt)
                try:
                    terminal.until(b'DYNAMIC_IO_OK', timeout=.25)
                except AssertionError as error:
                    if 'timeout waiting' not in str(error): raise
                else:
                    raise AssertionError('dynamic read returned before input was supplied')
                terminal.send(b'D\r')
                terminal.until(b'DYNAMIC_IO_OK')
                terminal.until(b'DYNAMIC_CHILD_OK')
                terminal.until(b'DYNAMIC_FORK_OK')
                terminal.until(b'CALLBACK_STATUS:0\n')
            if name == 'byteorder':
                terminal.until(b'SHORT_STATUS:101\n')
            terminal.until(shutdown, timeout=60)
            assert terminal.child.wait(timeout=30) == 0
            assert terminal.log.count(b'RUSTCELL_PANIC') == (3 if name == 'byteorder' else 2)
            assert terminal.log.count(b'PANIC_ARGUMENT') == 1
        finally:
            (output / f'{name}-{system}.log').write_bytes(terminal.log)
            terminal.close()
        check_image(system, image)
        compiler.run('node', HERE / 'tests/check-crates.mjs', system, image, expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', choices=['both', 'host', 'fuse-x', 'fuzix'], default='both')
    parser.add_argument('--crate', choices=CRATES, action='append', dest='crates')
    parser.add_argument('--fetch', action='store_true', help='download locked dependencies before testing')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--toolchain')
    args = parser.parse_args()
    output = args.build.resolve() / 'crates'
    output.mkdir(parents=True, exist_ok=True)
    selected = compiler.toolchain(args.toolchain)
    cargo = ['rustup', 'run', selected, 'cargo']
    if args.fetch:
        compiler.run(*cargo, 'fetch', '--manifest-path', PROJECT, '--locked')
    # Host references use ordinary rustc code generation and their own cache.
    environment = dict(os.environ)
    for variable in ['RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTC', 'RUSTC_WRAPPER',
                     'RUSTC_WORKSPACE_WRAPPER', 'CARGO_BUILD_TARGET', 'CARGO_TARGET_DIR']:
        environment.pop(variable, None)
    systems = ['fuse-x', 'fuzix'] if args.system == 'both' else ([] if args.system == 'host' else [args.system])
    results = dict(compiler_commit=compiler.COMMIT, packages=tomllib.loads(
        PROJECT.with_name('Cargo.lock').read_text())['package'], probes={})
    report = output / ('results.json' if args.system == 'both' else f'results-{args.system}.json')
    # A failed run must not leave a previous successful report in place.
    report.unlink(missing_ok=True)
    for name in args.crates or CRATES:
        print(f'{name}: building ordinary Rust reference', flush=True)
        reference = subprocess.run([*cargo, 'run', '--quiet', '--offline', '--locked',
                                    '--manifest-path', str(PROJECT), '--target-dir', str(output / 'host'),
                                    '--bin', 'rustcell-crates', '--no-default-features', '--features', name], env=environment,
                                   capture_output=True)
        (output / f'{name}-host.log').write_bytes(reference.stderr)
        if reference.returncode:
            raise RuntimeError(reference.stderr.decode(errors='replace'))
        expected = output / f'{name}.bin'
        expected.write_bytes(reference.stdout)
        assert reference.stdout, 'empty reference transcript'
        row = dict(bytes=len(reference.stdout), sha256=hashlib.sha256(reference.stdout).hexdigest(), guests={})
        for system in systems:
            print(f'{name}: building {system}; log: {output / (name + "-" + system + "-build.log")}', flush=True)
            app = output / (name + ('.fxe' if system == 'fuse-x' else '.rxe'))
            execute([sys.executable, HERE / 'cargo.py', '--manifest-path', PROJECT, '--offline', '--locked',
                     '--bin', 'rustcell-crates', '--no-default-features', '--features', name, '--target', system, '-o', app,
                     '--build', args.build.resolve(), '--compiler', args.compiler.resolve(),
                     '--fuzix-build', args.fuzix_build.resolve(), '--fuse-x-build', args.fuse_x_build.resolve(),
                     '--toolchain', selected], output / f'{name}-{system}-build.log')
            print(f'{name}: running {system}', flush=True)
            guest(system, name, app, expected, args)
            row['guests'][system] = dict(status='passed', executable_bytes=app.stat().st_size)
        if name == 'postcard-derived':
            row['derived_deserialization'] = 'round trips and truncated records passed'
        results['probes'][name] = row
    report.write_text(json.dumps(results, indent=2) + '\n')
    print(f'Crate compatibility results: {report}', flush=True)


if __name__ == '__main__':
    main()
