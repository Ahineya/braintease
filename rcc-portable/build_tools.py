#!/usr/bin/env python3
"""Build the portable assembler/linker for the host or either guest."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
HERE=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['host','fuzix','fuse-x'],default='host')
    p.add_argument('--backend',choices=['rust','portable'],default='rust')
    p.add_argument('--build',type=Path)
    args=p.parse_args();sources=sorted((HERE/'asm').glob('*.c'))
    if args.os=='host':
        flags=[]
        if sys.platform=='darwin':flags=['-isysroot',subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
        directory=HERE/'build';directory.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=directory,prefix='.tools-') as tmp:
            app=Path(tmp)/'rcc-as'
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'-std=c99','-O2','-Wall','-Wextra','-Wno-misleading-indentation',*map(str,sources),'-o',str(app)],check=True)
            os.replace(app,directory/'rcc-as')
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'-std=c99','-O2','-Wall','-Wextra',str(HERE/'driver/cc.c'),'-o',str(app)],check=True)
            os.replace(app,directory/'rcc-cc')
        shutil_copy(directory/'rcc-as',directory/'rcc-ld')
        print(directory/'rcc-as')
    else:
        ext='rxe' if args.os=='fuzix' else 'fxe'
        command=[sys.executable,str(HERE/('fuzix.py' if args.os=='fuzix' else 'fuse_x.py')),*map(str,sources),'-o',str(HERE/f'build/apps/rcc-as.{ext}')]
        command += ['-I', str(HERE/'guest/include')]
        if args.os=='fuzix':command+=['--backend',args.backend]
        elif args.backend=='portable':p.error('native guest builds still use the host backend')
        if args.build:command+=['--build',str(args.build)]
        subprocess.run(command,check=True);shutil_copy(HERE/f'build/apps/rcc-as.{ext}',HERE/f'build/apps/rcc-ld.{ext}')
        driver=[sys.executable,str(HERE/('fuzix.py' if args.os=='fuzix' else 'fuse_x.py')),str(HERE/'driver/cc.c'),'-o',str(HERE/f'build/apps/rcc-cc.{ext}')]
        driver += ['-I', str(HERE/'guest/include')]
        if args.os=='fuzix':driver+=['--backend',args.backend]
        if args.build:driver+=['--build',str(args.build)]
        subprocess.run(driver,check=True)

def shutil_copy(source,dest):
    import shutil
    temporary=dest.with_suffix(dest.suffix+'.new');shutil.copy2(source,temporary);os.replace(temporary,dest)

if __name__=='__main__':main()
