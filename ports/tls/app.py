#!/usr/bin/env python3
"""Link the guest TLS consumer against the appropriate Riplix userspace SDK."""
import argparse
import fcntl
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
from build import ROOT, PORT, build as build_library, shared_library


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--lowerer', type=Path, help='optional compatible compiler used only for IR lowering')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--tls-build', type=Path)
    parser.add_argument('--config', type=Path, help='directory containing a matching user_settings.h')
    parser.add_argument('--skip-sdk', action='store_true')
    parser.add_argument('--test-crypto', action='store_true')
    parser.add_argument('--source', type=Path, help='additional TLS/device acceptance program')
    parser.add_argument('-j', type=int, default=3)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    sdk = (args.build_dir or ROOT / ('fuse-x/build' if args.target == 'native' else 'build/fuzix')).resolve()
    tls = (args.tls_build or sdk / 'tls').resolve()
    tls.mkdir(parents=True, exist_ok=True)
    with (tls / (args.target + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        build_application(args)


def build_application(args):
    compiler = args.compiler.resolve()
    lowerer = (args.lowerer or compiler).resolve()
    sdk = (args.build_dir or ROOT / ('fuse-x/build' if args.target == 'native' else 'build/fuzix')).resolve()
    tls = (args.tls_build or sdk / 'tls').resolve()
    config = (args.config or PORT / 'config').resolve()
    inputs = build_library(tls, compiler, args.target, args.j, config)
    source = args.source.resolve() if args.source else PORT / 'tests/cryptocheck.c' if args.test_crypto else ROOT / 'ports/fuzix/upstream/Applications/netd/htget.c'
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    flags = ['-DRIPPLE_MMU=1', '-DNO_64BIT', '-DWOLFSSL_USER_SETTINGS']
    for directory in [PORT, config, tls / 'source', ROOT / 'ports/fuzix/ripple/include',
                      ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']:
        flags += ['-I', str(directory)]
    if args.target == 'native':
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(source), '--compiler', str(compiler),
                   '--linker', str(lowerer), '--build', str(sdk), '-o', str(output)]
        # cc.py accepts attached -I/-D arguments so argparse does not consume
        # them as options to the frontend driver.
        command += ['-DWOLFSSL_USER_SETTINGS'] + ['-I' + str(p) for p in [PORT, config, tls / 'source']]
        if os.environ.get('FUSE_X_SHARED_LIBRARIES', '1') != '0' and not args.test_crypto:
            command += ['--library', shared_library(inputs,sdk,compiler,lowerer)]
        else:
            for ir in inputs:
                command += ['--object-ir', str(ir)]
        subprocess.run(command, check=True)
    else:
        make = ['make', '-s', '--no-print-directory', '-C', str(ROOT / 'ports/fuzix'),
                f'BUILD={sdk}', f'RCC={compiler}']
        if not args.skip_sdk:
            subprocess.run([*make, f'-j{args.j}', 'rfl-sdk'], check=True)
        info = json.loads(subprocess.check_output([*make, 'rfl-sdk-info'], text=True))
        application = tls / 'ripple' / (source.stem + '.ir')
        result = subprocess.run([str(compiler), 'compile', str(source), '--emit-ir', *flags], capture_output=True)
        application.with_suffix('.log').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors='replace'))
        application.write_bytes(result.stdout)
        assembler = ROOT / 'src/ripple-asm/target/release/rasm'
        def lower(ir):
            assembly, obj = ir.with_suffix('.asm'), ir.with_suffix('.pobj')
            if obj.exists() and obj.stat().st_mtime_ns >= max(p.stat().st_mtime_ns for p in (ir, lowerer, assembler)):
                return obj
            with ir.with_suffix('.lower.log').open('w') as log:
                subprocess.run([str(lowerer), 'generate-asm', '-i', str(ir), '--banked-globals', '-o', str(assembly)], stdout=log, stderr=log, check=True)
                subprocess.run([str(assembler), 'assemble', str(assembly), '-o', str(obj), '--bank-size', '64000', '--max-immediate', '65535'], stdout=log, stderr=log, check=True)
            return obj
        with ThreadPoolExecutor(max_workers=args.j) as pool:
            objects = list(pool.map(lower, [application, *inputs]))
        with output.with_suffix('.link.log').open('w') as log:
            subprocess.run([sys.executable, str(ROOT/'ports/fuzix/tools/shared-link.py'), '--build', str(sdk), '--linker', str(assembler.with_name('rlink')), '--compiler', str(compiler), '--', info['entry'], *map(str, objects), *info['libraries'],
                            '-f', 'rxe', '--bank-size', '64000', '-o', str(output)], stdout=log, stderr=log, check=True)
    print(f'Built {output}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
