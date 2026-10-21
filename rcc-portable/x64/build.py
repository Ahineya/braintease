#!/usr/bin/env python3
"""Build the standalone portable x86_64 assembler (ELF64 relocatable output)."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guest', action='store_true', help='bootstrap a C16 guest executable')
    p.add_argument('--linker', type=Path)
    p.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    p.add_argument('--heap-profile', choices=['hosted','bounded'], default='hosted')
    p.add_argument('-o', type=Path)
    a = p.parse_args()
    output = a.o or HERE.parent / ('build/apps/rcc-x64-as.fxe' if a.guest else 'build/rcc-x64-as')
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = sorted(HERE.glob('*.c'))
    if a.guest:
        command = ['python3', str(ROOT / 'fuse-x/cc.py'), *map(str,sources),
                   str(HERE.parent / 'guest/strtoull.c'), '-I', str(HERE.parent / 'guest/include'), '--heap-profile', a.heap_profile,
                   '--build', str(a.build), '-o', str(output)]
        if a.linker: command += ['--static','--linker',str(a.linker)]
    else:
        command = [*shlex.split(os.environ.get('CC','cc')), '-std=c99','-O2','-Wall','-Wextra',
                   *map(str,sources),'-o',str(output)]
        if os.uname().sysname == 'Darwin':
            sdk = os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
            command += ['-isysroot',sdk]
    subprocess.run(command,check=True)
    print(output)


if __name__ == '__main__': main()
