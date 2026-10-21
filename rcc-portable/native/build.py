#!/usr/bin/env python3
"""Bootstrap the experimental C16 RCC IR -> QBE lowering tool."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0,str(HERE.parent))
from guest_build import FORMATTERS, support


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guest',action='store_true')
    p.add_argument('--build',type=Path,default=ROOT/'build/guest-native-assessment/x64-sdk')
    p.add_argument('-o',type=Path)
    a=p.parse_args(); output=a.o or HERE.parent/('build/apps/rcc-qbe-x86.fxe' if a.guest else 'build/rcc-qbe')
    sources=[*sorted(HERE.glob('*.c')),HERE.parent/'ripple/parse.c',HERE.parent/'ripple/helpers.c']
    output.parent.mkdir(parents=True,exist_ok=True)
    if a.guest:
        # The SDK has not added strtoull yet. Keep the declaration and 64-bit
        # formatter private to this bootstrap tool, as for the Ripple tools.
        wrapper=HERE.parent/'build/native-parser.c'
        wrapper.write_text('unsigned long long strtoull(const char *, char **, int);\n#include "'+str(HERE.parent/'ripple/parse.c')+'"\n')
        sources=[wrapper if s==HERE.parent/'ripple/parse.c' else s for s in sources]
        sources.append(support(HERE.parent/'build/guest-support'))
        command=['python3',str(ROOT/'fuse-x/cc.py'),*map(str,sources),str(HERE.parent/'guest/strtoull.c'),
                 '--heap-profile','bounded','--static','--linker',str(ROOT/'tools/x86_64/app_link.py'),
                 '--build',str(a.build),'-o',str(output)]
        for name in FORMATTERS: command+=['-D',name+'=rcc_c_'+name]
    else:
        command=[*shlex.split(os.environ.get('CC','cc')),'-std=c99','-O2','-Wall','-Wextra',*map(str,sources),'-o',str(output)]
        if os.uname().sysname=='Darwin':
            sdk=os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
            command+=['-isysroot',sdk]
    subprocess.run(command,check=True)
    print(output)


if __name__=='__main__': main()
