#!/usr/bin/env python3
"""Experimental rustc MIR -> RCC -> Riplix/Fuse-X compiler (one cell per Rust byte)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
COMMIT = '365c0e1d7a614ca94cb48431dcd2bc6d3b645db1'
PINNED = 'nightly-2026-05-07'


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, cwd=ROOT, **kwargs)


def toolchain(requested=None):
    """Reuse the installed nightly alias only if it is the exact pinned compiler."""
    candidates = [requested] if requested else [PINNED, 'nightly']
    for name in candidates:
        result = subprocess.run(['rustup', 'run', name, 'rustc', '-Vv'], capture_output=True, text=True)
        if result.returncode == 0 and f'commit-hash: {COMMIT}\n' in result.stdout:
            return name
    raise RuntimeError(f'This rustc-private prototype requires {PINNED} ({COMMIT[:9]}). '
                       f'Install with: rustup toolchain install {PINNED} --component rustc-dev,rust-src')


def prepare(build, selected, environment=None):
    target = HERE / 'targets/ripple-fuzix.json'
    rustc = ['rustup', 'run', selected, 'rustc']
    sysroot = Path(run(*rustc, '--print', 'sysroot', capture_output=True, text=True, env=environment).stdout.strip())
    if not list((sysroot / 'lib').glob('*rustc_driver*')):
        raise RuntimeError(f'Missing rustc-dev; run: rustup component add --toolchain {selected} rustc-dev')
    source = sysroot / 'lib/rustlib/src/rust/library/core/src/lib.rs'
    if not source.exists():
        raise RuntimeError(f'Missing rust-src; run: rustup component add --toolchain {selected} rust-src')
    print('Rust prototype: checking MIR driver...', file=sys.stderr, flush=True)
    run('rustup', 'run', selected, 'cargo', 'build', '--locked', '--manifest-path', HERE / 'Cargo.toml',
        '--target-dir', build / 'driver', env=environment)
    support_sources = sorted((HERE / 'runtime/rustcell').glob('*.rs'))
    fingerprint = hashlib.sha256(b'core+alloc+rustcell-v1' + COMMIT.encode() + target.read_bytes() +
                                (HERE / 'runtime/compiler_builtins.rs').read_bytes() +
                                b''.join(p.read_bytes() for p in support_sources)).hexdigest()[:24]
    guest_sysroot = build / ('sysroot-' + fingerprint)
    lib = guest_sysroot / 'lib/rustlib/ripple-fuzix/lib'
    if not (guest_sysroot / 'ready').exists():
        print('Rust prototype: building target core/alloc/rustcell metadata (no LLVM object code)...', file=sys.stderr, flush=True)
        build.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='sysroot-', dir=build) as temporary:
            stage = Path(temporary) / 'sysroot'
            stage_lib = stage / 'lib/rustlib/ripple-fuzix/lib'
            stage_lib.mkdir(parents=True)
            common = ['--crate-type', 'rlib', '--target', target, '-Zunstable-options', '--emit', 'metadata',
                      '-Cpanic=abort', '--cap-lints', 'allow']
            run(*rustc, HERE / 'runtime/compiler_builtins.rs', '--crate-name', 'compiler_builtins',
                *common, '-o', stage_lib / 'libcompiler_builtins.rmeta', env=environment)
            run(*rustc, source, '--crate-name', 'core', '--edition', '2024', *common,
                '-Zalways-encode-mir', '-Copt-level=1', '-o', stage_lib / 'libcore.rmeta', env=environment)
            run(*rustc, source.parents[2] / 'alloc/src/lib.rs', '--crate-name', 'alloc',
                '--edition', '2024', *common, '--sysroot', stage,
                '-Zalways-encode-mir', '-Copt-level=1', '-o', stage_lib / 'liballoc.rmeta', env=environment)
            run(*rustc, HERE / 'runtime/rustcell/lib.rs', '--crate-name', 'rustcell',
                '--edition', '2024', *common, '--sysroot', stage,
                '-Zalways-encode-mir', '-Copt-level=1', '-o', stage_lib / 'librustcell.rmeta', env=environment)
            (stage / 'ready').write_text(COMMIT + '\n')
            if guest_sysroot.exists():
                shutil.rmtree(guest_sysroot)
            stage.rename(guest_sysroot)
    if not all((lib / name).exists() for name in ['libcore.rmeta', 'liballoc.rmeta', 'librustcell.rmeta']):
        raise RuntimeError('incomplete target metadata cache; remove ' + str(guest_sysroot))
    return build / 'driver/debug/rust-rcc', guest_sysroot


def emit_ir(source, output, native, driver, guest_sysroot):
    args = [driver, source, '--crate-type', 'lib', '--crate-name', 'rustcell_application', '--edition', '2024',
            '--target', HERE / 'targets/ripple-fuzix.json', '--sysroot', guest_sysroot,
            '-Zunstable-options', '-Cpanic=abort', '-Copt-level=1', '-Coverflow-checks=off',
            '--extern', 'rustcell=' + str(guest_sysroot / 'lib/rustlib/ripple-fuzix/lib/librustcell.rmeta'),
            '--emit', 'metadata', '--rcc-output', output]
    if native:
        args.append('--rcc-native')
    run(*args)


def compile_c(compiler, source, output, log):
    args = [compiler, 'compile', source, '--target', 'ripple', '--bank-size', '64000',
            '-DRIPPLE_MMU=1', '-I', ROOT / 'ports/fuzix/ripple/include',
            '-I', ROOT / 'ports/fuzix/upstream/Library/include', '-I', ROOT / 'runtime/include']
    run(*args, '-o', output, stdout=log, stderr=subprocess.STDOUT)


def link(output, native, ir, compiler, fuzix_build, fuse_x_build, directory, log):
    def stage(*args):
        return run(*args, stdout=log, stderr=subprocess.STDOUT)

    if native:
        stage('python3', ROOT / 'fuse-x/cc.py', HERE / 'runtime/bridge.c', '--object-ir', ir,
            '--compiler', compiler, '--build', fuse_x_build, '-o', output)
        return
    # Obtain only the already-built SDK paths. No OS rebuild and no disk updates.
    sdk = run('make', '-s', '-C', ROOT / 'ports/fuzix', f'BUILD={fuzix_build}',
              'rfl-sdk-info', capture_output=True, text=True)
    objects = json.loads(sdk.stdout.strip().splitlines()[-1])
    assembly = directory / 'rust.asm'
    stage(compiler, 'generate-asm', '-i', ir, '-o', assembly)
    bridge = directory / 'bridge.asm'
    compile_c(compiler, HERE / 'runtime/bridge.c', bridge, log)
    assembler = ROOT / 'src/ripple-asm/target/release/rasm'
    linker = ROOT / 'src/ripple-asm/target/release/rlink'
    for name in ['rust', 'bridge']:
        stage(assembler, 'assemble', directory / f'{name}.asm', '--bank-size', '64000',
            '--max-immediate', '65535', '-o', directory / f'{name}.pobj')
    stage('python3', ROOT / 'ports/fuzix/tools/shared-link.py', '--build', fuzix_build,
        '--linker', linker, '--compiler', compiler, '--', objects['entry'], directory / 'rust.pobj', directory / 'bridge.pobj',
        *objects['libraries'], '-f', 'rxe', '--bank-size', '64000', '-o', output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('-o', '--output', type=Path, required=True)
    parser.add_argument('--target', choices=['fuzix', 'fuse-x'], default='fuzix')
    parser.add_argument('--emit-ir', action='store_true')
    parser.add_argument('--toolchain')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    build.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    driver, guest_sysroot = prepare(build, toolchain(args.toolchain))
    with tempfile.TemporaryDirectory(prefix='compile-', dir=build) as temporary:
        directory = Path(temporary)
        ir = directory / 'application.ir'
        emit_ir(args.source.resolve(), ir, args.target == 'fuse-x', driver, guest_sysroot)
        if args.emit_ir:
            os.replace(ir, output)
        else:
            executable = directory / 'application'
            log_path = build / f'{output.name}.{args.target}.link.log'
            print(f'Rust prototype: linking {args.target} (log: {log_path})', file=sys.stderr, flush=True)
            try:
                with log_path.open('w') as log:
                    link(executable, args.target == 'fuse-x', ir, args.compiler.resolve(),
                         args.fuzix_build.resolve(), args.fuse_x_build.resolve(), directory, log)
            except subprocess.CalledProcessError as error:
                raise RuntimeError(f'link failed; see {log_path}\n' + log_path.read_text(errors='replace')[-4000:]) from error
            os.replace(executable, output)
    print(f'Built Rust {args.target} {"IR" if args.emit_ir else "executable"}: {output}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'rust-rcc: {error}', file=sys.stderr)
        sys.exit(1)
