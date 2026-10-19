#!/usr/bin/env python3
"""Run the byteorder-based binary inspector against ordinary Rust on both guests."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import compile as compiler
from test import Terminal, check_image
from test_crates import execute

ROOT, HERE = compiler.ROOT, compiler.HERE
MANIFEST = HERE / 'examples/inspect/Cargo.toml'
LENGTHS = [0, 1, 15, 16, 17, 257, 20009]


def guest(system, app, references, args):
    build = args.fuzix_build.resolve() if system == 'fuzix' else args.fuse_x_build.resolve()
    output = args.build.resolve() / 'inspect'
    with tempfile.TemporaryDirectory(prefix='inspect-' + system + '-') as temporary:
        image = Path(temporary) / 'test.img'
        inputs = {f'/case{length}': dict(source=str(output / f'input{length}'), mode=0o100644)
                  for length in LENGTHS}
        compiler.run('node', HERE / 'tests/image.mjs', system, build, app, image, '', json.dumps(inputs))
        check_image(system, image)
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                           RCC_NATIVE_TIMEOUT='240', RIPPLE_NET='off')
        if system == 'fuzix':
            command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                       '--memory', '16384000', '--jit', '--net', 'off', '--audio', 'off', '--timeout', '240']
            prompt, shutdown = b'fuzix# ', b'Halted.'
        else:
            command = [build / 'fuse-x', '--run', image]
            prompt, shutdown = b'fuse-x# ', b'filesystem synced; session stopped'
        terminal = Terminal(command, environment)
        try:
            terminal.until(prompt)
            for length in LENGTHS:
                terminal.send(f'rustprobe /case{length} > /dump{length};echo INSPECT_{length}:$?\r'.encode())
                terminal.until(f'INSPECT_{length}:0\n'.encode(), timeout=120)
                terminal.until(prompt)
            terminal.send(b'rustprobe /missing;echo MISSING:$?;rustprobe;echo USAGE:$?;shutdown\r')
            terminal.until(b'MISSING:1\n')
            terminal.until(b'USAGE:2\n')
            terminal.until(shutdown, timeout=60)
            assert terminal.child.wait(timeout=30) == 0
            assert b'RUSTCELL_PANIC' not in terminal.log
        finally:
            (output / f'{system}.log').write_bytes(terminal.log)
            terminal.close()
        check_image(system, image)
        for length, expected in references.items():
            compiler.run('node', HERE / 'tests/check-crates.mjs', system, image, expected, f'/dump{length}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', choices=['both', 'fuse-x', 'fuzix'], default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--toolchain')
    args = parser.parse_args()
    output = args.build.resolve() / 'inspect'
    output.mkdir(parents=True, exist_ok=True)
    report = output / ('results.json' if args.system == 'both' else f'results-{args.system}.json')
    report.unlink(missing_ok=True)
    selected = compiler.toolchain(args.toolchain)
    environment = dict(os.environ)
    for variable in ['RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTC', 'RUSTC_WRAPPER',
                     'RUSTC_WORKSPACE_WRAPPER', 'CARGO_BUILD_TARGET', 'CARGO_TARGET_DIR']:
        environment.pop(variable, None)
    references = {}
    for length in LENGTHS:
        source, expected = output / f'input{length}', output / f'expected{length}'
        source.write_bytes(bytes(i % 256 for i in range(length)))
        result = subprocess.run(['rustup', 'run', selected, 'cargo', 'run', '--quiet', '--offline', '--locked',
                                 '--manifest-path', str(MANIFEST), '--target-dir', str(output / 'host'), '--', str(source)],
                                env=environment, capture_output=True)
        assert result.returncode == 0, result.stderr.decode(errors='replace')
        assert result.stdout.count(b'\n') == (length + 15) // 16
        if length:
            assert result.stdout.startswith(b'00000000  00 ')
        expected.write_bytes(result.stdout)
        references[length] = expected
    systems = ['fuse-x', 'fuzix'] if args.system == 'both' else [args.system]
    results = dict(compiler_commit=compiler.COMMIT, input_bytes=LENGTHS, guests={})
    for system in systems:
        print(f'binspect: building {system}; log: {output / (system + "-build.log")}', flush=True)
        app = output / ('binspect.fxe' if system == 'fuse-x' else 'binspect.rxe')
        execute([sys.executable, HERE / 'cargo.py', '--manifest-path', MANIFEST, '--offline', '--locked',
                 '--target', system, '-o', app, '--build', args.build.resolve(), '--compiler', args.compiler.resolve(),
                 '--fuzix-build', args.fuzix_build.resolve(), '--fuse-x-build', args.fuse_x_build.resolve(),
                 '--toolchain', selected], output / f'{system}-build.log')
        print(f'binspect: running {system}', flush=True)
        guest(system, app, references, args)
        results['guests'][system] = dict(status='passed', executable_bytes=app.stat().st_size)
    report.write_text(json.dumps(results, indent=2) + '\n')
    print(f'Binary inspector results: {report}', flush=True)


if __name__ == '__main__':
    main()
