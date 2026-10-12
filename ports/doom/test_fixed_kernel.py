#!/usr/bin/env python3
"""Validate assembled fixed-point arithmetic on the unchanged Rust VM."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=PORT / 'build/fixed-kernel-test')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = PORT / 'src/fixed_fast.asm'
    if not source.is_file():
        raise RuntimeError(f'missing assembly kernel: {source}')
    # Keep the exact validated source alongside the binary and report.
    (out / 'fixed_test.asm').write_bytes(source.read_bytes())
    commands = [
        [ROOT / 'src/ripple-asm/target/release/rasm', 'assemble', out / 'fixed_test.asm',
         '-o', out / 'fixed_test.pobj', '-b', '64000'],
        [ROOT / 'src/ripple-asm/target/release/rlink', out / 'fixed_test.pobj',
         '-o', out / 'fixed_test.bin', '-b', '64000'],
        ['rustc', '--edition=2021', '-O', PORT / 'tools/test_fixed_kernel.rs',
         '--extern', f'rvm={ROOT / "target/release/librvm.rlib"}',
         '-L', f'dependency={ROOT / "target/release/deps"}', '-o', out / 'verify-fixed'],
        [out / 'verify-fixed', out],
    ]
    for command in commands:
        subprocess.run([str(part) for part in command], cwd=ROOT, check=True, timeout=180)
    result_path = out / 'results.json'
    result = json.loads(result_path.read_text())
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result.update(assembly_sha256=digest(out / 'fixed_test.asm'),
                  binary_sha256=digest(out / 'fixed_test.bin'),
                  vm_library_sha256=digest(ROOT / 'target/release/librvm.rlib'),
                  multiply_oracle='low32(arithmetic_shift_right(int64(int32(a)) * int64(int32(b)), 16))',
                  division_oracle='Doom magnitude overflow guard, then int64(a)*65536/int64(b), truncated toward zero')
    result_path.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
