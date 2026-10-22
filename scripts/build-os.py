#!/usr/bin/env python3
"""Incremental host-tool and OS builds; never opens or formats a guest disk."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
from build_cache import cached_step, sources

ROOT = Path(__file__).resolve().parent.parent


def run(command):
    subprocess.run(list(map(str, command)), check=True, cwd=ROOT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('os', choices=['fuzix', 'fuse-x'])
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--rebuild', action='store_true', help='bypass launcher cache and force Make recipes')
    args = parser.parse_args()
    build = args.build.resolve();build.mkdir(parents=True, exist_ok=True)
    system = 'Riplix' if args.os == 'fuzix' else 'Fuse-X'
    if args.os == 'fuse-x':
        os.environ.setdefault('FUSE_X_SHARED_LIBRARIES', '1')
    print('Build: Checking expired artifact cleanup (at most once per day)', flush=True)
    cleanup = subprocess.run(['python3', str(ROOT / 'scripts/clean-artifacts.py'),
                              '--apply', '--daily', '--quiet'])
    if cleanup.returncode:
        print('Build: Artifact cleanup skipped; continuing with the build', flush=True)
    with (build / '.build.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f'Build: Waiting for another build in {build}', flush=True)
            fcntl.flock(lock, fcntl.LOCK_EX)
        print('Build: Checking host tool inputs and cached outputs', flush=True)
        rust = [p for p in ROOT.glob('rcc-*') if (p / 'Cargo.toml').exists()]
        rust += [ROOT / 'rfl', ROOT / 'rvm', ROOT / 'Cargo.toml', ROOT / 'Cargo.lock',
                 ROOT / '.cargo', ROOT / 'src/ripple-asm', ROOT / 'libraries/graphics']
        inputs = sources(*rust, ROOT / 'scripts/build-os.py', ROOT / 'scripts/build_cache.py')
        settings = {key: os.environ.get(key) for key in ['RUSTFLAGS', 'CARGO_PROFILE_RELEASE_OPT_LEVEL',
                    'CC', 'CXX', 'CFLAGS', 'LDFLAGS', 'DEVELOPER_DIR', 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET',
                    'CODE_BANKS', 'BULK_IO', 'MMU', 'MAKEFLAGS', 'RUSTUP_TOOLCHAIN', 'FUSE_X_SHARED_LIBRARIES', 'RIPLIX_SHARED_LIBRARIES',
                    'CARGO_ENCODED_RUSTFLAGS', 'RUSTC']}
        settings.update({key: value for key, value in os.environ.items()
                         if key.startswith('CARGO_PROFILE_RELEASE_')})
        settings['rustc-version'] = subprocess.check_output(
            [os.environ.get('RUSTC', 'rustc'), '-vV'], text=True).strip()
        tools = [ROOT / 'target/release/rcc', ROOT / 'target/release/rfl',
                 ROOT / 'src/ripple-asm/target/release/rasm', ROOT / 'src/ripple-asm/target/release/rlink']
        tools += [ROOT / 'target/release/rvm'] if args.os == 'fuzix' else [ROOT / 'target/release/librcc_native_runtime.a']
        def host_tools():
            print('Build: Building host compilers and '+('RVM' if args.os == 'fuzix' else 'native runtime'), flush=True)
            packages = ['rcc-driver', 'rfl', 'rvm' if args.os == 'fuzix' else 'rcc-native-runtime']
            command = ['cargo', 'build', '--release', '--manifest-path', ROOT / 'Cargo.toml']
            for package in packages:command += ['-p', package]
            run(command)
            print('Build: Checking/building host assembler and linker', flush=True)
            run(['cargo', 'build', '--release', '--manifest-path', ROOT / 'src/ripple-asm/Cargo.toml', '--bin', 'rasm', '--bin', 'rlink'])
        changed = cached_step(build / '.incremental-tools.json', inputs,
                              [args.os, settings, str(Path(__file__).resolve())], host_tools, tools,
                              force=args.rebuild)
        print('Host tools: '+('checked/updated' if changed else 'current'), flush=True)
        print(f'Build: Checking {system} sources and cached outputs', flush=True)
        roots = [ROOT / 'ports/fuzix', ROOT / 'ports/basic', ROOT / 'runtime', ROOT / 'ports/nanox', ROOT / 'ports/graphics',
                 ROOT / 'ports/fusede', ROOT / 'ports/lynx', ROOT / 'ports/netsurf', ROOT / 'ports/tls', ROOT / 'ports/telegram', ROOT / 'ports/music', ROOT / 'ports/chat', ROOT / 'rfl',
                 ROOT / 'libraries', ROOT / 'rcc-portable', ROOT / 'c-test/examples/rOS',
                 ROOT / 'scripts/build-os.py', ROOT / 'scripts/build_cache.py']
        if args.os == 'fuse-x':roots += [ROOT / 'fuse-x', ROOT / 'ports/lvgl']
        dependencies = [*sources(*roots), *tools]
        def artifacts():
            files = [build / 'compiler/manifest.json', *[p for p in (build / 'compiler').rglob('*') if p.is_file()]]
            if args.os == 'fuzix':
                files += [build / n for n in ['FUZIX.SYS', 'bios.bin', 'apps.json', '.mmu', '.code-banks', '.bulk-io']]
                files += [build / (n+'.rxe') for n in json.loads((build / 'apps.json').read_text())]
                files += list((build / 'lib').glob('*.rsl.[0-9]'))
            else:
                # Ignore collection binaries left by the former default build;
                # these optional shared-folder apps are not OS artifacts.
                optional = {'puzzles', *[g['id'] for g in json.loads((ROOT / 'ports/puzzles/catalog.json').read_text())]}
                files += [build / 'fuse-x', build / 'application-report.json', build / 'apps/telegram.fxe', build / 'apps/music.fxe', build / 'apps/wavplay.fxe']
                files += [p for p in (build / 'apps').glob('*.fxe') if p.stem not in optional]
                files += list((build / 'lib').glob('*.fxl.[0-9]'))
                if os.uname().sysname == 'Darwin':files += [build / 'Fuse-X.app/Contents/MacOS/fuse-x', build / 'Fuse-X.app/Contents/Info.plist']
            return files
        def operating_system():
            print(f'Build: Building {system} kernel, applications and guest C tools', flush=True)
            command = ['make', '-C', ROOT / ('ports/fuzix' if args.os == 'fuzix' else 'fuse-x'),
                       f'BUILD={build}', '-j4']
            if args.rebuild:
                command += ['-B'];os.environ['RIPPLE_REBUILD'] = '1'
                if args.os == 'fuse-x':
                    # Clear shared IR once, rather than recompiling the same
                    # libc/graphics units again for every application.
                    shutil.rmtree(build / 'sdk', ignore_errors=True)
                    for path in (build / 'nanox-native').glob('*.ir'):
                        path.unlink()
            command += ['images' if args.os == 'fuzix' else 'all']
            run(command)
            print('Build: Verifying built outputs and saving the build cache', flush=True)
        os_settings = dict(settings)
        if args.os == 'fuse-x':
            # These change native kernel/SDK/package code, not the host Rust
            # tools. The outer cache must reach Make's mode/profile stamps.
            os_settings['RCC_NATIVE_MEMORY'] = os.environ.get('RCC_NATIVE_MEMORY', 'checked')
            os_settings['FUSE_X_HEAP_PROFILE'] = os.environ.get('FUSE_X_HEAP_PROFILE', 'hosted')
            os_settings['FUSE_X_GRAPHITE'] = os.environ.get('FUSE_X_GRAPHITE', '0')
        changed = cached_step(build / '.incremental-os.json', dependencies,
                              [args.os, str(build), os_settings], operating_system, artifacts,
                              force=args.rebuild)
        print(system+': '+('updated' if changed else 'current (no compilation needed)'), flush=True)


if __name__ == '__main__':main()
