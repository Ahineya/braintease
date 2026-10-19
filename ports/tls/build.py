#!/usr/bin/env python3
"""Compile the shared guest TLS library with the unchanged RCC C16 ABI."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from adapt import prepare
from vendor import CRYPTO

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
SOURCES = ['wolfcrypt/src/' + n + '.c' for n in CRYPTO if n not in ('asn_orig', 'asn_tsp')]
SOURCES += ['src/' + n + '.c' for n in ('ssl', 'tls', 'tls13', 'internal', 'keys', 'wolfio')]
SOURCES += ['@/ripple_tls.c', '@/ripple_crypto.c']


def build(output, compiler, target='native', jobs=3, config=None):
    output, compiler = Path(output).resolve(), Path(compiler).resolve()
    config = Path(config or PORT / 'config').resolve()
    source = prepare(output / 'source')
    flags = ['--target', target, '-DRIPPLE_MMU=1', '-DNO_64BIT', '-DWOLFSSL_USER_SETTINGS']
    includes = [PORT, config, source, ROOT / 'ports/fuzix/ripple/include',
                ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include',
                ROOT / 'ports/fuzix/ripple']
    digest = hashlib.sha256(str((compiler, compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for path in sorted(directory.glob('*.h') if directory == PORT else directory.rglob('*.h')):
            digest.update(str(path.relative_to(directory)).encode())
            digest.update(path.read_bytes())
        flags += ['-I', str(directory)]
    base = digest.digest()
    objects = output / target
    objects.mkdir(parents=True, exist_ok=True)
    def compile_one(name):
        path = PORT / name[2:] if name.startswith('@/') else source / name
        ir = objects / (path.stem + '.ir')
        stamp = ir.with_suffix('.json')
        key = hashlib.sha256(base + path.read_bytes()).hexdigest()
        try:
            cached = ir.exists() and ir.stat().st_size and json.loads(stamp.read_text()).get('key') == key
        except (OSError, ValueError):
            cached = False
        if not cached:
            result = subprocess.run([str(compiler), 'compile', str(path), *flags, '--emit-ir'],
                                    capture_output=True, env=dict(os.environ, RUST_LOG='error'))
            ir.with_suffix('.log').write_bytes(result.stderr)
            if result.returncode:
                raise RuntimeError(f'{name}:\n{result.stderr.decode(errors="replace")}')
            if not ir.exists() or ir.read_bytes() != result.stdout:
                with tempfile.NamedTemporaryFile(dir=objects, delete=False) as temporary:
                    temporary.write(result.stdout)
                    temporary_path = temporary.name
                os.replace(temporary_path, ir)
            stamp.write_text(json.dumps(dict(key=key)) + '\n')
        return ir
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        result = list(pool.map(compile_one, SOURCES))
    (objects / 'objects.json').write_text(json.dumps(list(map(str, result)), indent=2) + '\n')
    print(f'Compiled {len(result)} guest TLS objects ({target})', flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/tls')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--config', type=Path, help='directory containing a matching user_settings.h')
    parser.add_argument('-j', '--jobs', type=int, default=3)
    args = parser.parse_args()
    try:
        build(args.build, args.compiler, args.target, args.jobs, args.config)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
