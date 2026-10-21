#!/usr/bin/env python3
"""Build the opt-in guest SSH library against the existing C16 crypto port."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from adapt import prepare

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _build(output, compiler, target='native', jobs=3, client=False):
    output, compiler = Path(output).resolve(), Path(compiler).resolve()
    ssh = prepare(output / 'source')
    crypto_adapter = load('ssh_crypto_adapt', ROOT / 'ports/tls/adapt.py')
    crypto = crypto_adapter.prepare(output / 'crypto')
    # wolfCrypt sign-extends octet masks via signed char. RCC's signed char
    # is 16-bit, so spell that octet sign extension explicitly (without branches).
    misc=crypto/'wolfcrypt/src/misc.c'
    text=misc.read_text();old='(signed int)(signed char)m'
    if text.count(old)!=4:raise RuntimeError('wolfCrypt mask helper shape changed')
    crypto_adapter.write_changed(misc,text.replace(old,'((int)(((unsigned)m & 255) ^ 128) - 128)'))
    extra_crypto = json.loads((PORT / 'crypto/source.json').read_text())['files']
    for name, expected in extra_crypto.items():
        source = (PORT / 'crypto' / name).read_bytes()
        if hashlib.sha256(source).hexdigest() != expected:
            raise RuntimeError(f'Pinned extra wolfCrypt source changed: {name}')
        text, _ = crypto_adapter.octet_casts(source.decode())
        crypto_adapter.write_changed(crypto / 'wolfcrypt/src' / name, text)
    vendor = load('ssh_crypto_vendor', ROOT / 'ports/tls/vendor.py')
    sources = [crypto / f'wolfcrypt/src/{name}.c' for name in vendor.CRYPTO
               if name not in ('asn_orig', 'asn_tsp')]
    sources += [crypto / 'wolfcrypt/src' / name for name in extra_crypto]
    sources += [ssh / f'src/{name}.c' for name in ('ssh', 'internal', 'log', 'io', 'port', 'misc', 'ossh', 'wolfsftp')]
    sources += [ROOT / 'ports/tls/ripple_crypto.c', PORT / 'platform.c']
    if client:sources += [PORT/'keys.c']
    includes = ([PORT/'config/client'] if client else [])+[PORT, PORT / 'config', ssh, crypto, ROOT / 'ports/tls',
                ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']
    flags = ['--target', target, '-DRIPPLE_MMU=1', '-DNO_64BIT', '-DWOLFSSL_USER_SETTINGS']
    for directory in includes:
        flags += ['-I', str(directory)]
    fingerprint = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for header in sorted(directory.rglob('*.h')):
            fingerprint.update(str(header).encode());fingerprint.update(header.read_bytes())
    fingerprint.update((ROOT / 'ports/tls/config/user_settings.h').read_bytes())
    objects = output / target
    objects.mkdir(parents=True, exist_ok=True)

    def compile_one(source):
        prefix = 'ssh-' if source.is_relative_to(ssh) else 'crypto-' if source.is_relative_to(crypto) else ''
        result = objects / f'{prefix}{source.stem}.ir'
        stamp = result.with_suffix('.sha256')
        key = hashlib.sha256(fingerprint.digest() + source.read_bytes()).hexdigest()
        if not result.exists() or not stamp.exists() or stamp.read_text() != key:
            print(f'Compiling SSH {prefix}{source.stem}', flush=True)
            command = subprocess.run([str(compiler), 'compile', str(source), *flags, '--emit-ir'], capture_output=True,
                                     env=dict(os.environ,RUST_LOG='error'))
            result.with_suffix('.log').write_bytes(command.stderr)
            if command.returncode:
                raise RuntimeError(f'{source}:\n{command.stderr.decode(errors="replace")}')
            result.write_bytes(command.stdout)
            stamp.write_text(key)
        return result

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects_list = list(pool.map(compile_one, sources))
    (objects / 'objects.json').write_text(json.dumps(list(map(str, objects_list)), indent=2) + '\n')
    return objects_list


def build(output, compiler, target='native', jobs=3, client=False):
    # Native and Ripple use the same generated source directory. Serialize
    # preparation and IR compilation so neither sees the other's intermediate
    # unadapted source during a concurrent cross-target build.
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=True)
    with (output/'source.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        return _build(output,compiler,target,jobs,client)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/ssh')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('-j', type=int, default=3)
    args = parser.parse_args()
    try:
        print(f'Compiled {len(build(args.build,args.compiler,args.target,args.j))} SSH objects')
    except (OSError, RuntimeError) as error:
        raise SystemExit(str(error))
