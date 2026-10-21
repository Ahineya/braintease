#!/usr/bin/env python3
"""Explicitly build Portable Puzzles, separately from the Fuse-X OS build."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/puzzles-fuse-x')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--only', nargs='+', help='build selected games or the puzzles launcher')
    args = parser.parse_args()
    names = ['puzzles', *[g['id'] for g in json.loads((ROOT / 'ports/puzzles/catalog.json').read_text())]]
    if args.only and set(args.only) - set(names):
        parser.error('unknown puzzle name')
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/graphics.py'),
                    '--build', str(args.build.resolve()), '--compiler', str(args.compiler.resolve()),
                    '--only', *(args.only or names)], check=True)


if __name__ == '__main__':
    main()
