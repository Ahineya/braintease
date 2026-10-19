#!/usr/bin/env python3
"""Compile a pinned wolfCrypt SHA-256 unit with RCC's actual C16 headers."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/tls-audit/probe')
    args = parser.parse_args()
    source, compiler, output = args.source.resolve(), args.compiler.resolve(), args.output.resolve()
    metadata = json.loads((PORT / 'AUDIT.json').read_text())
    for name, expected in metadata['files'].items():
        actual = hashlib.sha256((source / name).read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit(f'{name}: differs from the audited {metadata["version"]} source')
    output.mkdir(parents=True, exist_ok=True)
    flags = ['-DRIPPLE_MMU=1', '-DWOLFSSL_USER_SETTINGS']
    for path in [PORT / 'audit', source, ROOT / 'ports/fuzix/ripple/include',
                 ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include',
                 ROOT / 'ports/fuzix/ripple']:
        flags += ['-I', str(path)]
    result = subprocess.run([str(compiler), 'compile', str(source / 'wolfcrypt/src/sha256.c'),
                             '--target', args.target, '--emit-ir', *flags], capture_output=True)
    (output / 'sha256.log').write_bytes(result.stderr)
    ir = output / 'sha256.ir'
    if result.returncode == 0:
        ir.write_bytes(result.stdout)
    else:
        ir.unlink(missing_ok=True)
    report = dict(version=metadata['version'], target=args.target, char_bits=16,
                  int_bits=16, long_bits=32, compiled=result.returncode == 0,
                  returncode=result.returncode, diagnostic=result.stderr.decode(errors='replace'))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
