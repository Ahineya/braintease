#!/usr/bin/env python3
"""Validate actual assembled wall/floor raster leaves on the Rust VM only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=PORT / 'build/raster-kernel-test')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'raster_test.asm').write_bytes((PORT / 'src/raster_fast.asm').read_bytes())
    disk = out / 'fixture-disk.bin'
    disk.write_bytes(bytes(65536))
    commands = [
        [sys.executable, PORT / 'build.py', 'raster_kernels', '--target', 'both'],
        [PORT / 'build/native/raster_kernels/raster_kernels'],
        [ROOT / 'target/release/rvm', PORT / 'build/ripple/raster_kernels/raster_kernels.bin',
         '--bank-size', '64000', '--memory', '16384000', '--disk', disk,
         '--headless', '--exit-code', '--max-instructions', '1000000000', '--timeout', '120'],
        [ROOT / 'src/ripple-asm/target/release/rasm', 'assemble', out / 'raster_test.asm',
         '-o', out / 'raster_test.pobj', '-b', '64000'],
        [ROOT / 'src/ripple-asm/target/release/rlink', out / 'raster_test.pobj',
         '-o', out / 'raster_test.bin', '-b', '64000'],
        ['rustc', '--edition=2021', '-O', PORT / 'tools/test_raster_kernels.rs',
         '--extern', f'rvm={ROOT / "target/release/librvm.rlib"}',
         '-L', f'dependency={ROOT / "target/release/deps"}', '-o', out / 'verify-raster'],
        [out / 'verify-raster', out],
    ]
    for command in commands:
        subprocess.run([str(part) for part in command], cwd=ROOT, check=True, timeout=180)
    result_path = out / 'results.json'
    result = json.loads(result_path.read_text())
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result.update(assembly_sha256=digest(out / 'raster_test.asm'),
                  binary_sha256=digest(out / 'raster_test.bin'),
                  vm_library_sha256=digest(ROOT / 'target/release/librvm.rlib'),
                  oracle_sha256=digest(PORT / 'tools/test_raster_kernels.rs'),
                  column_oracle='map[source[(position >> 16) & 127]], position += step modulo 2^32, stride 320',
                  span_oracle='map[source[((position >> 4) & 4032) | (position >> 26)]], position += step modulo 2^32, stride 1',
                  low_detail_oracle='same sampled value written twice at adjacent output cells; column stride 320, span stride 2')
    result_path.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
