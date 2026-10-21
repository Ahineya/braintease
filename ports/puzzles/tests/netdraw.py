#!/usr/bin/env python3
"""Check real C16 Net rendering against the unmodified upstream engine."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/puzzles'
sys.path.insert(0,str(PORT))
from adapt import generate, COMMON
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    output=ROOT/'build/puzzles-tests'; output.mkdir(parents=True,exist_ok=True)
    generated=generate(ROOT/'fuse-x/build/puzzles/src')
    host=output/'netdraw-host'; exe=ROOT/'fuse-x/build/apps/netdraw.fxe'
    sources=[PORT/'tests/netdraw.c', PORT/'platform.c']
    sdk=[]
    if sys.platform=='darwin':
        sdk=['-isysroot',subprocess.check_output(['/usr/bin/xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
    subprocess.run([os.environ.get('CC','/usr/bin/cc' if sys.platform=='darwin' else 'cc'),*sdk,
                    '-std=c99','-D_POSIX_C_SOURCE=200809L','-DNO_TGMATH_H=1',
                    '-I',str(PORT/'upstream'),*map(str,sources),
                    *[str(PORT/'upstream'/f'{n}.c') for n in COMMON],'-lm','-o',str(host)],check=True)
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),*map(str,sources),
                    *[str(generated/f'{n}.c') for n in COMMON],'-I',str(generated),
                    '-D','NO_TGMATH_H=1','--float-format','--static','-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='puzzles-netdraw-') as directory:
        g=Guest(Path(directory),'native',{'netdraw':exe})
        try:
            for params,size in [('5x5',32),('7x7w',47),('11x11b0.3',64)]:
                expected=subprocess.check_output([str(host),params,str(size)],text=True).strip()
                result=g.shell(f'netdraw {params} {size}',seconds=180)
                observed='\n'.join(line for line in result.splitlines() if re.match(r'^(NET_DRAW|FAIL)\b',line))
                assert observed==expected,(params,size,observed,expected)
                assert 'failures=0' in observed,observed
                print(params,size,'upstream-identical drawing commands and cache; rotations, animation, locks, cursor, idle passed',flush=True)
        finally:
            (output/'netdraw.log').write_bytes(g.log)
            g.close()


if __name__=='__main__': main()
