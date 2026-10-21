#!/usr/bin/env python3
"""Compare real C16 engines with unmodified upstream compiled by the host C compiler."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/puzzles'
sys.path.insert(0,str(PORT))
from adapt import generate,COMMON,EXTRA
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
CASES=dict(mines=['9x9n10#123456789','16x16n40#987654321','30x16n99#123456789'],
           solo=['2x2#123456789','3x3#123456789','4x4db#987654321'],
           net=['5x5#123456789','7x7w#987654321','11x11#123456789'])

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--only',nargs='+',choices=list(CASES));a=ap.parse_args()
    output=ROOT/'build/puzzles-tests';output.mkdir(parents=True,exist_ok=True)
    generated=generate(ROOT/'fuse-x/build/puzzles/src');apps={}
    expected={}
    hostflags=[]
    if sys.platform=='darwin':
        hostflags=['-isysroot',subprocess.check_output(['/usr/bin/xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
    for name in a.only or CASES:
        units=[*COMMON,*EXTRA[name],name]
        host=output/f'{name}-host'
        subprocess.run([os.environ.get('CC','/usr/bin/cc' if sys.platform=='darwin' else 'cc'),*hostflags,'-std=c99','-D_POSIX_C_SOURCE=200809L','-DNO_TGMATH_H=1','-I',str(PORT/'upstream'),str(PORT/'tests/enginecheck.c'),str(PORT/'platform.c'),*[str(PORT/'upstream'/f'{n}.c') for n in units],'-lm','-o',str(host)],check=True)
        for case in CASES[name]:expected[name,case]=subprocess.check_output([str(host),case],text=True)
        exe=ROOT/'fuse-x/build/apps'/f'{name}check.fxe';apps[f'{name}check']=exe
        subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'tests/enginecheck.c'),str(PORT/'platform.c'),*[str(generated/f'{n}.c') for n in units],'-I',str(generated),'-D','NO_TGMATH_H=1','--float-format','--static','-o',str(exe)],check=True)
    with tempfile.TemporaryDirectory(prefix='puzzles-engines-') as directory:
        g=Guest(Path(directory),'native',apps)
        try:
            for name,case in expected:
                result=g.shell(f'{name}check {case}',seconds=300)
                observed='\n'.join(line for line in result.splitlines() if re.match(r'^(RANDOM|BOARD|SAVE|FAIL|PUZZLES_ENGINE)\b',line))+'\n'
                # Saved elapsed time differs for Mines; compare board/PRNG and
                # assert the actual save/restore/undo checks on both platforms.
                stable=lambda s:'\n'.join(x for x in s.splitlines() if not x.startswith('SAVE '))
                assert stable(observed)==stable(expected[name,case]),(name,case,observed,expected[name,case])
                assert 'failures=0' in observed,(name,case,observed)
                print(name,case,'upstream-identical board and PRNG; solve, undo/redo, save/load passed',flush=True)
        except BaseException:
            os.write(g.master,b'\x03')
            try:g.read(g.prompt,20)
            except AssertionError:pass
            raise
        finally:(output/'engines.log').write_bytes(g.log);g.close()

if __name__=='__main__':main()
