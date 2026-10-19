#!/usr/bin/env python3
"""Build the portable Ripple backend for the host, Fuse-X or Riplix/RVM."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from guest_build import FORMATTERS, support
HERE=Path(__file__).resolve().parent

def ensure_host():
    output=HERE/'build/rcc-ripple'
    inputs=[* (HERE/'ripple').glob('*.[ch]'),Path(__file__).resolve()]
    if not output.exists() or any(p.stat().st_mtime_ns>output.stat().st_mtime_ns for p in inputs):
        subprocess.run([sys.executable,str(HERE/'build_ripple.py')],check=True,stdout=sys.stderr)
    return output

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os',choices=['host','fuse-x','fuzix'],default='host')
    parser.add_argument('-o',type=Path)
    parser.add_argument('--build',type=Path)
    parser.add_argument('--backend',choices=['rust','portable'],default='rust',help='backend used to compile the Riplix tool itself')
    args=parser.parse_args()
    if args.backend=='portable' and args.os!='fuzix':parser.error('the portable backend currently builds Riplix/Ripple executables')
    output=(args.o or HERE/('build/rcc-ripple' if args.os=='host' else 'build/apps/rcc-ripple.'+('fxe' if args.os=='fuse-x' else 'rxe'))).resolve()
    output.parent.mkdir(parents=True,exist_ok=True)
    sources=sorted((HERE/'ripple').glob('*.c'))
    if args.os=='host':
        flags=[]
        if sys.platform=='darwin':
            sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
            flags=['-isysroot',sdk]
        with tempfile.TemporaryDirectory(prefix='.ripple-build-',dir=output.parent) as temp:
            compiled=Path(temp)/'rcc-ripple'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'-std=c99','-O2','-Wall','-Wextra','-Wno-misleading-indentation',*map(str,sources),'-o',str(compiled)],check=True)
            os.replace(compiled,output)
        print(output)
    else:
        command=[sys.executable,str(HERE/('fuse_x.py' if args.os=='fuse-x' else 'fuzix.py')),*map(str,sources),str(HERE/'guest/strtoull.c'),str(support(HERE/'build/guest-support')),'-I',str(HERE/'guest/include'),'-DRIPPLE_FLOAT_FORMAT','--float-format','-o',str(output)]
        if args.os=='fuzix':command+=['--backend',args.backend]
        if args.build:command+=['--build',str(args.build)]
        for name in FORMATTERS: command+=['-D',f'{name}=rcc_c_{name}']
        subprocess.run(command,check=True)
if __name__=='__main__':main()
