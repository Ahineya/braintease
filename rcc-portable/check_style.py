#!/usr/bin/env python3
"""Check maintained portable C sources; --format applies the local style.

Vendor snapshots, generated build files and compiler input fixtures are excluded.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', action='store_true')
    parser.add_argument('--clang-format', help='path to clang-format (defaults to PATH or Xcode)')
    args = parser.parse_args()
    formatter = args.clang_format or shutil.which('clang-format')
    if not formatter and sys.platform == 'darwin':
        result = subprocess.run(['xcrun', '--find', 'clang-format'], capture_output=True, text=True)
        if result.returncode == 0:
            formatter = result.stdout.strip()
    if not formatter:
        parser.error('clang-format is required; pass its path with --clang-format')
    files = [HERE / 'preprocessor.c', HERE / 'native-format.h']
    for directory in ['src', 'ripple', 'asm', 'driver', 'guest', 'qbe', 'x64', 'native', 'x64-link', 'native-driver', 'native-archive', 'native-sdk']:
        files += sorted((HERE / directory).glob('*.[ch]'))
    command = [formatter, *(['-i'] if args.format else ['--dry-run', '--Werror']), *map(str, files)]
    subprocess.run(command, check=True)
    print(f'{len(files)} maintained C files '+('formatted' if args.format else 'match the local style'))


if __name__ == '__main__':
    main()
