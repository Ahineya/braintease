#!/usr/bin/env python3
"""Build versioned Ripple shared modules from normal RCC object files."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from shared import sdk
from shared.link import pack

ROOT=Path(__file__).resolve().parents[3]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('inputs',type=Path,nargs='*')
    p.add_argument('-o','--output',type=Path,required=True)
    p.add_argument('--soname',default='')
    p.add_argument('--export',dest='exports',action='append')
    p.add_argument('--library',action='append',default=[],metavar='HOST=GUEST')
    p.add_argument('--startup',type=Path)
    p.add_argument('--cc',action='store_true',help='compile C sources using the shared guest SDK')
    p.add_argument('--sdk-only',action='store_true')
    p.add_argument('--curses',action='store_true')
    p.add_argument('--build',type=Path,default=ROOT/'build/riplix-shared')
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('-I',dest='includes',action='append',default=[])
    p.add_argument('-D',dest='defines',action='append',default=[])
    p.add_argument('--linker',type=Path,default=ROOT/'src/ripple-asm/target/release/rlink')
    p.add_argument('--assembler',type=Path,default=ROOT/'src/ripple-asm/target/release/rasm')
    a=p.parse_args()
    if a.cc or a.sdk_only:
        libraries,entry=sdk.prepare(a.build,a.compiler.resolve(),a.assembler.resolve(),a.linker.resolve(),a.curses)
        if a.sdk_only:return
        with tempfile.TemporaryDirectory(prefix='riplix-cc-') as folder:
            objects=[]
            includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',ROOT/'ports/fuzix/ripple',ROOT/'ports/graphics/include',*map(Path,a.includes)]
            for i,source in enumerate(a.inputs):
                assembly=Path(folder)/f'{i}.asm';obj=assembly.with_suffix('.pobj')
                subprocess.run([str(a.compiler.resolve()),'compile',str(source),'-o',str(assembly),'--bank-size','64000',
                                '-DRIPPLE_MMU=1',*['-I'+str(p) for p in includes],*['-D'+d for d in a.defines]],check=True,stdout=subprocess.DEVNULL)
                subprocess.run([str(a.assembler.resolve()),'assemble',str(assembly),'-o',str(obj),'--bank-size','64000','--max-immediate','65535'],check=True,stdout=subprocess.DEVNULL)
                objects.append(obj)
            pack(objects,a.output,linker=a.linker.resolve(),assembler=a.assembler.resolve(),name=a.soname,exports=a.exports,
                 dependencies=[*libraries,*[v.split('=',1) for v in a.library]],startup=entry if not a.soname else None)
        return
    if not a.inputs:p.error('choose object files, --cc sources, or --sdk-only')
    pack(a.inputs,a.output,linker=a.linker.resolve(),assembler=a.assembler.resolve(),name=a.soname,
         exports=a.exports,dependencies=[v.split('=',1) for v in a.library],startup=a.startup)


if __name__=='__main__':main()
