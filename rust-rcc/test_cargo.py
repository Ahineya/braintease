#!/usr/bin/env python3
"""Check Cargo features, host tools, incremental changes, and both guest formats."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import compile as compiler
from test import Terminal, check_image

ROOT, HERE = compiler.ROOT, compiler.HERE


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def guest(system, apps, args):
    build = args.fuzix_build.resolve() if system == 'fuzix' else args.fuse_x_build.resolve()
    output = args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='cargo-guest-') as temporary:
        image = Path(temporary) / 'test.img'
        extras = {'/bin/cargoloud': str(apps['loud']), '/bin/cargoedited': str(apps['edited'])}
        compiler.run('node', HERE / 'tests/image.mjs', system, build, apps['base'], image, '', json.dumps(extras))
        check_image(system, image)
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                           RCC_NATIVE_TIMEOUT='120', RIPPLE_NET='off')
        if system == 'fuzix':
            command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                       '--memory', '16384000', '--jit', '--net', 'off', '--audio', 'off', '--timeout', '120']
            prompt, shutdown = b'fuzix# ', b'Halted.'
        else:
            command = [build / 'fuse-x', '--run', image]
            prompt, shutdown = b'fuse-x# ', b'filesystem synced; session stopped'
        terminal = Terminal(command, environment)
        try:
            terminal.until(prompt)
            terminal.send(b'rustprobe;echo CBASE:$?;cargoloud;echo CFEATURE:$?;'
                          b'cargoedited;echo CEDIT:$?;shutdown\r')
            for marker in [b'RUSTCELL_CARGO_BASE\n', b'CBASE:0\n',
                           b'RUSTCELL_CARGO_LOUD\n', b'CFEATURE:0\n',
                           b'RUSTCELL_CARGO_EDIT\n', b'CEDIT:0\n', shutdown]:
                terminal.until(marker)
            assert terminal.child.wait(timeout=30) == 0
            assert b'RUSTCELL_PANIC' not in terminal.log
        finally:
            (output / ('cargo-' + system + '.log')).write_bytes(terminal.log)
            terminal.close()
        check_image(system, image)
    print(f'{system}: Cargo dependencies, features, build scripts, procedural macro and edited library ran correctly', flush=True)


def check_system(system, project, args):
    suffix = '.fxe' if system == 'fuse-x' else '.rxe'
    apps = {name: args.build.resolve() / ('cargo-' + name + suffix) for name in ['base', 'loud', 'edited']}
    common = [sys.executable, str(HERE / 'cargo.py'), '--manifest-path', str(project / 'Cargo.toml'),
              '--target', system, '--offline', '--locked', '--build', str(args.build.resolve()),
              '--compiler', str(args.compiler.resolve()), '--fuzix-build', str(args.fuzix_build.resolve()),
              '--fuse-x-build', str(args.fuse_x_build.resolve())]

    def build(name, *extra, fail=None):
        result = subprocess.run([*common, '-o', str(apps.get(name, args.build.resolve() / name)), *extra],
                                capture_output=True, text=True)
        with (args.build.resolve() / 'cargo-build-tests.log').open('a') as log:
            log.write(f'\n{system} / {name} / {extra}\n' + result.stdout + result.stderr)
        text = result.stdout + result.stderr
        if fail:
            assert result.returncode != 0 and fail in text, text[-6000:]
        else:
            assert result.returncode == 0, text[-6000:]
        return text

    library = project / 'math/src/lib.rs'
    original = library.read_text()
    build('base')
    base_hash, base_time = digest(apps['base']), apps['base'].stat().st_mtime_ns
    assert 'reusing linked output' in build('base')
    assert (digest(apps['base']), apps['base'].stat().st_mtime_ns) == (base_hash, base_time)
    build('loud', '--features', 'loud')
    assert digest(apps['loud']) != base_hash
    assert 'reusing linked output' in build('base')
    assert digest(apps['base']) == base_hash
    library.write_text(original.replace('RUSTCELL_CARGO_BASE', 'RUSTCELL_CARGO_EDIT'))
    build('edited')
    assert digest(apps['edited']) != base_hash
    library.write_text(original)
    assert 'reusing linked output' in build('base')
    assert digest(apps['base']) == base_hash

    # Cargo checks library code with the normal frontend, including unused code.
    library.write_text(original + '\npub fn invalid() { let mut v=0; let a=&mut v; let b=&mut v; *a+=*b; }\n')
    invalid_output = args.build.resolve() / 'invalid'
    invalid_output.write_bytes(b'previous valid output')
    build('invalid', fail='cannot borrow')
    assert invalid_output.read_bytes() == b'previous valid output'
    library.write_text(original)
    script = project / 'math/build.rs'
    saved_script = script.read_text()
    script.write_text(saved_script.replace('fn main() {', 'fn main() {\nprintln!("cargo:rustc-link-lib=unsupported_native");'))
    build('native-library', fail='native target libraries/link arguments are unsupported')
    script.write_text(saved_script)
    root = project / 'app/src/main.rs'
    saved_root = root.read_text()
    root.write_text(saved_root.replace('rust_entry() -> u16', 'rust_entry() -> u32'))
    build('invalid-entry', fail='entry must have signature fn rust_entry() -> u16')
    root.write_text(saved_root)
    build('release', '--release', '--all-features')
    print(f'{system}: unchanged/feature-switch/edit/restore builds and rejection checks passed', flush=True)
    guest(system, apps, args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', choices=['both', 'fuzix', 'fuse-x'], default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    args.build.resolve().mkdir(parents=True, exist_ok=True)
    systems = ['fuse-x', 'fuzix'] if args.system == 'both' else [args.system]
    # Spaces exercise command/dep-path forwarding; the fixture never needs a registry.
    with tempfile.TemporaryDirectory(prefix='cargo workspace ', dir=args.build.resolve()) as temporary:
        project = Path(temporary) / 'project'
        shutil.copytree(HERE / 'examples/cargo', project)
        for system in systems:
            check_system(system, project, args)
    (args.build.resolve() / ('cargo-results.json' if args.system == 'both' else f'cargo-results-{args.system}.json')).write_text(
        json.dumps(dict(compiler_commit=compiler.COMMIT, guests=systems, incremental='passed',
                        host_tools='passed', dependency_edits='passed', diagnostics='passed'), indent=2) + '\n')


if __name__ == '__main__':
    main()
