#!/usr/bin/env python3
"""Build/cache and stage the guest C compiler and SDK for normal OS images."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import cached_step, sources


def run(*args):
    env = dict(os.environ)
    # A parent make -B must not force a complete SDK rebuild for every compiler
    # translation unit. The outer SDK prerequisites have already been checked.
    env.pop('MAKEFLAGS', None)
    env.pop('MFLAGS', None)
    subprocess.run(list(map(str, args)), check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['fuzix', 'fuse-x'], required=True)
    parser.add_argument('--build', type=Path, help='OS build directory receiving compiler/')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--no-build', action='store_true', help='stage already built tools and SDK')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--install', type=Path, help='also update this stopped disk, with a backup')
    args = parser.parse_args()
    build = (args.build or (ROOT / ('build/fuzix' if args.os == 'fuzix' else 'fuse-x/build'))).resolve()
    sdk_build = args.fuzix_build.resolve()
    if args.os == 'fuzix':sdk_build = build
    HERE.joinpath('build').mkdir(exist_ok=True)
    with (HERE / 'build/.package.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        ext = 'rxe' if args.os == 'fuzix' else 'fxe'
        app = HERE / 'build/apps'
        tools = {'cc': app / f'rcc-cc.{ext}', 'rcc-c': app / f'rcc-c.{ext}',
                 'rcc-ripple': app / ('rcc-ripple-self.rxe' if ext == 'rxe' else 'rcc-ripple.fxe'),
                 'rcc-as': app / f'rcc-as.{ext}', 'rcc-ld': app / f'rcc-ld.{ext}'}
        sdk = HERE / 'build/sdk'
        if not args.no_build:
            cache = HERE / 'build/package-cache'
            environment = {name: os.environ.get(name) for name in
                           ['CC', 'CFLAGS', 'LDFLAGS', 'DEVELOPER_DIR', 'SDKROOT',
                            'MACOSX_DEPLOYMENT_TARGET', 'MMU', 'CODE_BANKS', 'BULK_IO']}
            if args.os == 'fuse-x':
                environment['FUSE_X_SHARED_LIBRARIES'] = os.environ.get('FUSE_X_SHARED_LIBRARIES', '1')
            else:
                environment['RIPLIX_SHARED_LIBRARIES'] = os.environ.get('RIPLIX_SHARED_LIBRARIES', '1')
            def step(name, inputs, command, outputs):
                changed = cached_step(cache / (name + '.json'), inputs, [command, environment],
                                      lambda: run(*command), outputs,
                                      force=args.force or os.environ.get('RIPPLE_REBUILD') == '1')
                print(f'Guest compiler {name}: '+('built' if changed else 'current'), flush=True)
            frontend = sources(HERE / 'upstream', HERE / 'src', HERE / 'preprocessor.c',
                               HERE / 'adapt.py', HERE / 'build.py')
            step('host-frontend', frontend, [sys.executable, HERE / 'build.py'], [HERE / 'build/rcc-c'])
            step('host-backend', sources(HERE / 'ripple', HERE / 'build_ripple.py'),
                 [sys.executable, HERE / 'build_ripple.py'], [HERE / 'build/rcc-ripple'])
            shared = sources(HERE / 'guest', HERE / 'cc.py', HERE / 'guest_build.py',
                             HERE / 'fuzix.py', HERE / 'fuse_x.py', ROOT / 'runtime/include',
                             ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/ripple/libc',
                             ROOT / 'ports/fuzix/upstream/Library', ROOT / 'ports/fuzix/upstream/Kernel/include',
                             ROOT / 'ports/fuzix/Makefile', ROOT / 'ports/fuzix/ripple/float-format.inc',
                             ROOT / 'ports/fuzix/tools/shared-link.py', ROOT / 'ports/fuzix/tools/shared',
                             ROOT / 'ports/fuzix/ripple/exec-args.h',
                             ROOT / 'ports/fuzix/ripple/user-entry.asm', ROOT / 'ports/fuzix/ripple/userlib.c',
                             ROOT / 'fuse-x/cc.py', ROOT / 'fuse-x/libc')
            binaries = [ROOT / 'target/release/rcc', ROOT / 'src/ripple-asm/target/release/rasm',
                        ROOT / 'src/ripple-asm/target/release/rlink']
            if ext == 'fxe':binaries += [ROOT / 'target/release/librcc_native_runtime.a']
            guest_inputs = [*shared, *binaries, HERE / 'build/rcc-c']
            step(args.os+'-frontend', [*frontend, *guest_inputs],
                 [sys.executable, HERE / 'guest_build.py', '--os', args.os, '--build', build], [tools['rcc-c']])
            backend = [sys.executable, HERE / 'build_ripple.py', '--os', args.os, '--build', build]
            if ext == 'rxe':backend += ['--backend', 'portable', '-o', tools['rcc-ripple']]
            step(args.os+'-backend', [*guest_inputs, *sources(HERE / 'ripple', HERE / 'build_ripple.py')],
                 backend, [tools['rcc-ripple']])
            command = [sys.executable, HERE / 'build_tools.py', '--os', args.os, '--build', build]
            if ext == 'rxe':command += ['--backend', 'portable']
            step(args.os+'-assembler', [*guest_inputs, *sources(HERE / 'asm', HERE / 'driver', HERE / 'build_tools.py'), HERE / 'build/rcc-ripple'],
                 command, [tools[n] for n in ['cc', 'rcc-as', 'rcc-ld']])
            def sdk_command():
                run('make', '-s', '-C', ROOT / 'runtime', 'BANK_SIZE=64000', 'crt0.pobj', 'libruntime.par')
                run(sys.executable, HERE / 'sdk.py', '--float', '--build', sdk_build)
            # Both guests use the same Ripple SDK. The native runtime is a
            # dependency of FXE tools, not of these Ripple libraries.
            sdk_inputs = [*shared, *binaries[:3], HERE / 'sdk.py', *sources(ROOT / 'runtime', ROOT / 'ports/fuzix/ripple')]
            changed = cached_step(cache / ('sdk-'+str(sdk_build).replace('/', '_')+'.json'), sdk_inputs,
                                  [str(sdk_build), 'float', '64000', environment], sdk_command,
                                  lambda: [p for p in sdk.rglob('*') if p.is_file()],
                                  force=args.force or os.environ.get('RIPPLE_REBUILD') == '1')
            print('Guest compiler SDK: '+('built' if changed else 'current'), flush=True)
        missing = [p for p in tools.values() if not p.is_file()]
        if missing or not (sdk / 'libc.rpa').is_file():
            raise RuntimeError('missing compiler artifacts; run without --no-build: '+str(missing))
        destination = build / 'compiler'
        files = {f'bin/{name}': path for name, path in tools.items()}
        files.update({'usr/lib/rcc/'+str(p.relative_to(sdk)): p for p in sdk.rglob('*') if p.is_file()})
        from source import prepare, sdk_identity
        source = prepare()
        files['usr/lib/rcc/sdk-id'] = sdk_identity(sdk)
        from build_cache import fingerprint
        seed_id = HERE / ('build/seed-id-' + args.os)
        content = fingerprint(tools.values()) + '\n'
        if not seed_id.exists() or seed_id.read_text() != content:seed_id.write_text(content)
        files['usr/lib/rcc/seed-id'] = seed_id
        files.update({'usr/src/rcc/'+str(p.relative_to(source)): p for p in source.rglob('*') if p.is_file()})
        entries = []
        for relative, source in sorted(files.items()):
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            data = source.read_bytes()
            if not output.exists() or output.read_bytes() != data:
                temporary = output.with_name(output.name+'.new')
                temporary.write_bytes(data);os.replace(temporary, output)
            executable = relative.startswith('bin/')
            output.chmod(0o755 if executable else 0o644)
            entries.append({'name': '/'+relative, 'file': relative, 'mode': 0o100755 if executable else 0o100644})
        manifest = destination / 'manifest.json'
        content = json.dumps(entries, indent=2)+'\n'
        if not manifest.exists() or manifest.read_text() != content:manifest.write_text(content)
        print(f'Compiler package ready: {destination}', flush=True)
        if args.install:
            from install import install
            install(args.install.resolve(), args.os, destination)


if __name__ == '__main__':
    try:main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
