#!/usr/bin/env python3
"""Use Cargo metadata/feature resolution, then lower reachable MIR through RCC."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import compile as compiler

HERE, ROOT = compiler.HERE, compiler.ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
from build_cache import c_dependencies


def select_binary(metadata, manifest, package, binary):
    packages = [p for p in metadata['packages'] if p['id'] in metadata['workspace_members']]
    if package:
        packages = [p for p in packages if p['name'] == package or p['id'] == package]
    elif any(Path(p['manifest_path']).resolve() == manifest for p in packages):
        packages = [p for p in packages if Path(p['manifest_path']).resolve() == manifest]
    else:
        packages = [p for p in packages if p['id'] in metadata['workspace_default_members']]
    candidates = [(p, t) for p in packages for t in p['targets']
                  if 'bin' in t['kind'] and (not binary or t['name'] == binary)]
    if len(candidates) != 1:
        raise RuntimeError('select one application with --package and --bin; candidates: ' +
                           ', '.join(p['name'] + '/' + t['name'] for p, t in candidates))
    return candidates[0]


def cargo_check(command, environment, package, binary):
    artifact = None
    child = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, text=True)
    for line in child.stdout:
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            print(line, end='', file=sys.stderr)
            continue
        if message.get('reason') == 'compiler-message':
            print(message['message'].get('rendered') or message['message']['message'],
                  end='', file=sys.stderr)
        if (message.get('reason') == 'compiler-artifact' and message['package_id'] == package['id']
                and message['target']['name'] == binary['name'] and 'bin' in message['target']['kind']):
            artifact = message
    if child.wait():
        raise RuntimeError('Cargo check failed; see diagnostics above')
    if not artifact:
        raise RuntimeError('Cargo did not report the selected binary metadata')
    return artifact


def link_fingerprint(ir, native, rcc, fuzix_build, fuse_x_build):
    digest = hashlib.sha256(b'cargo-rcc-link-v1' + ir.read_bytes())
    inputs = set((HERE / 'runtime').glob('*.c')) | {HERE / 'compile.py', HERE / 'cargo.py'}
    for directory in [ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple/include',
                      ROOT / 'ports/fuzix/upstream/Library/include']:
        inputs.update(directory.rglob('*.h'))
    tools = [rcc]
    if native:
        inputs.update((ROOT / 'ports/fuzix/ripple/libc').glob('*.c'))
        inputs.update((ROOT / 'ports/fuzix/upstream/Library/libs').glob('*.c'))
        inputs.update((ROOT / 'libraries/unicode').glob('*.[ch]'))
        inputs.update([ROOT / 'fuse-x/cc.py', ROOT / 'fuse-x/libc/bridge.c',
                       ROOT / 'ports/fuzix/ripple/userlib.c', ROOT / 'scripts/build_cache.py',
                       ROOT / 'ports/fuzix/upstream/Kernel/include/userstructs.h'])
        # Superset of direct/local include dependencies used by the SDK sources.
        inputs.update((ROOT / 'ports/fuzix/ripple').rglob('*.h'))
        inputs.update((ROOT / 'ports/graphics/include').rglob('*.h'))
        inputs.update((ROOT / 'libraries/ini').glob('*.h'))
        inputs.update((ROOT / 'libraries/tui-theme').glob('*.h'))
    else:
        sdk = compiler.run('make', '-s', '-C', ROOT / 'ports/fuzix', f'BUILD={fuzix_build}',
                           'rfl-sdk-info', capture_output=True, text=True)
        objects = json.loads(sdk.stdout.strip().splitlines()[-1])
        inputs.update(Path(p) for p in [objects['entry'], *objects['libraries']])
        tools += [ROOT / 'src/ripple-asm/target/release/rasm', ROOT / 'src/ripple-asm/target/release/rlink']
    includes = [ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple', ROOT / 'ports/graphics/include',
                ROOT / 'libraries/ini', ROOT / 'libraries/tui-theme']
    for source in list(inputs):
        if source.suffix in ('.c', '.h'):
            inputs.update(c_dependencies(source, includes))
    for path in sorted(inputs):
        digest.update(str(path).encode())
        digest.update(path.read_bytes())
    for path in tools:
        info = path.stat()
        digest.update(str((path, info.st_size, info.st_mtime_ns)).encode())
    digest.update(str((native, fuzix_build, fuse_x_build)).encode())
    return digest.hexdigest()


def replace_if_changed(source, output):
    if output.exists() and source.read_bytes() == output.read_bytes():
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    # Stage in the destination filesystem so --output may be on another volume.
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as file:
        temporary = Path(file.name)
        file.write(source.read_bytes())
    try:
        temporary.chmod(source.stat().st_mode & 0o777)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest-path', type=Path, default=Path('Cargo.toml'))
    parser.add_argument('-p', '--package')
    parser.add_argument('--bin')
    parser.add_argument('--features', action='append', default=[])
    parser.add_argument('--no-default-features', action='store_true')
    parser.add_argument('--all-features', action='store_true')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--locked', action='store_true')
    parser.add_argument('--release', action='store_true')
    parser.add_argument('--target', choices=['fuzix', 'fuse-x'], default='fuzix')
    parser.add_argument('-o', '--output', type=Path)
    parser.add_argument('--emit-ir', action='store_true')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--toolchain')
    args = parser.parse_args()
    manifest, build = args.manifest_path.resolve(), args.build.resolve()
    selected = compiler.toolchain(args.toolchain)
    # Do not inherit another compiler wrapper/sysroot or target directory into
    # either the prototype driver build or this isolated Cargo check.
    environment = dict(os.environ)
    for name in ['RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'RUSTC_WRAPPER', 'RUSTC_WORKSPACE_WRAPPER',
                 'RUSTC', 'CARGO_BUILD_TARGET', 'CARGO_TARGET_DIR']:
        environment.pop(name, None)
    environment['RUSTC'] = compiler.run('rustup', 'which', '--toolchain', selected, 'rustc',
                                       capture_output=True, text=True).stdout.strip()
    environment['RUSTC_WORKSPACE_WRAPPER'] = ''
    cargo = ['rustup', 'run', selected, 'cargo', '-Zjson-target-spec']
    resolution = (['--offline'] if args.offline else []) + (['--locked'] if args.locked else [])
    features = [item for value in args.features for item in ['--features', value]]
    if args.no_default_features: features.append('--no-default-features')
    if args.all_features: features.append('--all-features')
    metadata = subprocess.run([*cargo, 'metadata', '--format-version=1', '--no-deps',
                               '--manifest-path', str(manifest), *resolution, *features],
                              env=environment, capture_output=True, text=True)
    if metadata.returncode:
        raise RuntimeError('Cargo metadata failed:\n' + metadata.stderr)
    package, binary = select_binary(json.loads(metadata.stdout), manifest, args.package, args.bin)
    driver, sysroot = compiler.prepare(build, selected, environment)
    key = hashlib.sha256(str((manifest, package['id'], binary['name'])).encode()).hexdigest()[:16]
    cache = build / 'cargo' / key / args.target
    cache.mkdir(parents=True, exist_ok=True)
    target = HERE / 'targets/ripple-fuzix.json'
    environment.update(RUSTC_WRAPPER=str(HERE / 'cargo_wrapper.py'),
                       RUSTCELL_CARGO_SOURCE=binary['src_path'], RUSTCELL_CARGO_RECORDS=str(cache / 'records'),
                       RUSTCELL_CARGO_DRIVER=str(driver), RUSTCELL_CARGO_NATIVE='1' if args.target == 'fuse-x' else '0')
    flags = ['-Zunstable-options', '-Zalways-encode-mir', '-Cpanic=abort', '-Copt-level=1',
             '-Coverflow-checks=off', '--sysroot', str(sysroot), '--extern',
             'rustcell=' + str(sysroot / 'lib/rustlib/ripple-fuzix/lib/librustcell.rmeta')]
    # Cargo tracks target rustflags. A backend update must regenerate the root
    # IR even when the Rust sources and existing metadata are otherwise fresh.
    backend = hashlib.sha256((HERE / 'cargo_wrapper.py').read_bytes())
    for source in sorted((HERE / 'src').rglob('*.rs')): backend.update(source.read_bytes())
    backend.update(str((driver.stat().st_size, driver.stat().st_mtime_ns)).encode())
    flags += ['--cfg', 'rustcell_backend="' + backend.hexdigest() + '"',
              '--check-cfg', 'cfg(rustcell_backend,values(any()))']
    environment['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(flags)
    command = [*cargo, 'check', '--manifest-path', str(manifest), '--target', str(target),
               '--target-dir', str(cache / 'target'), '-p', package['name'], '--bin', binary['name'],
               '--message-format=json', *resolution, *features]
    if args.release: command.append('--release')
    artifact = cargo_check(command, environment, package, binary)
    metadata_files = [Path(p) for p in artifact['filenames'] if p.endswith('.rmeta')]
    if len(metadata_files) != 1:
        raise RuntimeError('expected one Cargo binary metadata artifact')
    emitted_ir = cache / 'records' / (metadata_files[0].stem.removeprefix('lib') + '.ir')
    if not emitted_ir.exists():
        raise RuntimeError('missing Cargo MIR output; remove ' + str(cache / 'target') + ' and retry')
    native = args.target == 'fuse-x'
    output = args.output.resolve() if args.output else cache / (binary['name'] +
                          ('.ir' if args.emit_ir else '.fxe' if native else '.rxe'))
    with tempfile.TemporaryDirectory(prefix='lower-', dir=cache) as temporary:
        directory = Path(temporary)
        ir = directory / 'application.ir'
        ir.write_bytes(emitted_ir.read_bytes())
        if args.emit_ir:
            replace_if_changed(ir, output)
            print(f'Cargo → RCC IR: {output}', flush=True)
            return
        rcc, fuzix_build, fuse_x_build = args.compiler.resolve(), args.fuzix_build.resolve(), args.fuse_x_build.resolve()
        fingerprint = link_fingerprint(ir, native, rcc, fuzix_build, fuse_x_build)
        linked = cache / 'linked' / fingerprint
        linked.mkdir(parents=True, exist_ok=True)
        executable = linked / ('application.fxe' if native else 'application.rxe')
        stamp = linked / 'linked.json'
        try:
            state = json.loads(stamp.read_text())
        except (OSError, ValueError):
            state = {}
        if state.get('fingerprint') == fingerprint and executable.exists() and state.get('sha256') == hashlib.sha256(executable.read_bytes()).hexdigest():
            print('Cargo → RCC: guest executable unchanged; reusing linked output', flush=True)
        else:
            log_path = cache / 'link.log'
            print(f'Cargo → RCC: linking {args.target} (log: {log_path})', flush=True)
            with log_path.open('w') as log:
                try:
                    compiler.link(directory / 'application', native, ir, rcc, fuzix_build, fuse_x_build, directory, log)
                except subprocess.CalledProcessError as error:
                    raise RuntimeError(f'link failed; see {log_path}\n' + log_path.read_text(errors='replace')[-4000:]) from error
            os.replace(directory / 'application', executable)
            pending_stamp = directory / 'linked.json'
            pending_stamp.write_text(json.dumps(dict(fingerprint=fingerprint, sha256=hashlib.sha256(executable.read_bytes()).hexdigest())) + '\n')
            replace_if_changed(pending_stamp, stamp)
        replace_if_changed(executable, output)
    print(f'Built Cargo Rust {args.target} executable: {output}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'rust-rcc Cargo: {error}', file=sys.stderr)
        sys.exit(1)
