#!/usr/bin/env python3
"""Build worker; run.py owns the build lock and live progress reporter."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def build_profile(build):
    steps = [
        ('Building RCC and RBF host tools',
         ['cargo', 'build', '--offline', '--release', '--jobs', '2',
          '-p', 'rbf', '-p', 'rcc-driver']),
        ('Building assembler and linker',
         ['cargo', 'build', '--offline', '--release', '--jobs', '2', '--manifest-path',
          str(ROOT / 'src/ripple-asm/Cargo.toml'), '--bin', 'rasm', '--bin', 'rlink']),
        ('Building FUZIX boot kernel and images',
         ['make', '-C', str(ROOT / 'ports/fuzix/rbf'), '-j2', f'BUILD={build}']),
    ]
    for phase, command in steps:
        print(f'Build: {phase}', flush=True)
        result = subprocess.run(command, cwd=ROOT)
        if result.returncode:
            return result.returncode if result.returncode > 0 else 128 - result.returncode
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    raise SystemExit(build_profile(parser.parse_args().build.resolve()))
