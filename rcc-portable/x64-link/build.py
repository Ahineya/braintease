#!/usr/bin/env python3
"""Bootstrap the portable RXE1 linker and native guest driver."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guest',action='store_true')
    p.add_argument('--driver',action='store_true')
    p.add_argument('--archiver',action='store_true')
    p.add_argument('--build',type=Path,default=ROOT/'build/guest-native-assessment/x64-sdk')
    p.add_argument('-o',type=Path)
    a=p.parse_args()
    if a.driver and a.archiver:p.error('choose either driver or archiver')
    name='rcc-native-cc' if a.driver else 'rcc-x64-ar' if a.archiver else 'rcc-x64-ld'
    output=a.o or HERE.parent/('build/apps/'+name+'-x86.fxe' if a.guest else 'build/'+name)
    sources=[HERE.parent/'native-driver/cc.c'] if a.driver else [HERE.parent/'native-archive/main.c'] if a.archiver else sorted(HERE.glob('*.c'))
    output.parent.mkdir(parents=True,exist_ok=True)
    if a.guest:
        command=['python3',str(ROOT/'fuse-x/cc.py'),*map(str,sources),'--heap-profile','bounded',
                 '--static','--linker',str(ROOT/'tools/x86_64/app_link.py'),
                 '--build',str(a.build),'-o',str(output)]
    else:
        command=[*shlex.split(os.environ.get('CC','cc')),'-std=c99','-O2','-Wall','-Wextra',
                 *map(str,sources),'-o',str(output)]
        if os.uname().sysname=='Darwin':
            command+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()]
    subprocess.run(command,check=True)
    print(output)


if __name__=='__main__':main()
