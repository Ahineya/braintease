#!/usr/bin/env python3
"""Build the shared browser cache, then publish a normal guest application."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=['native', 'ripple'], required=True)
    p.add_argument('--compiler', type=Path, default=ROOT/'target/release/rcc')
    p.add_argument('-j', '--jobs', type=int, default=3)
    p.add_argument('-o', '--output', type=Path, required=True)
    a = p.parse_args()
    source = ROOT/'build/netsurf'/('netsurf.fxe' if a.target=='native' else 'netsurf.rxe')
    subprocess.run([sys.executable, str(ROOT/'ports/netsurf/app.py'), '--gui', '--target', a.target,
                    '--compiler', str(a.compiler.resolve()), '-j', str(a.jobs), '-o', str(source)], check=True)
    output = a.output.resolve()
    if output == source:
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.netsurf-', dir=output.parent) as folder:
        staged = Path(folder)/output.name
        shutil.copy2(source, staged)
        staged.replace(output)


if __name__ == '__main__':
    main()
