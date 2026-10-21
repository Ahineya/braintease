#!/usr/bin/env python3
"""All engines: compare C16 generation, rendering and moves to original upstream."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3];PORT=ROOT/'ports/puzzles'
sys.path.insert(0,str(PORT))
from adapt import GAMES, COMMON, HELPERS, generate
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--only',nargs='+',choices=[g['id'] for g in GAMES]);ap.add_argument('--reuse-probe',action='store_true')
    ap.add_argument('--seed',default='123456789');a=ap.parse_args()
    output=ROOT/'build/puzzles-tests/collection';output.mkdir(parents=True,exist_ok=True)
    results=output/('seed-'+hashlib.sha256(a.seed.encode()).hexdigest()[:12]);results.mkdir(exist_ok=True)
    generated=generate(ROOT/'fuse-x/build/puzzles/src')
    names=COMMON+HELPERS+[g['id'] for g in GAMES]
    host=output/'host';exe=ROOT/'fuse-x/build/apps/collectioncheck.fxe'
    sdk=[]
    if sys.platform=='darwin':sdk=['-isysroot',subprocess.check_output(['/usr/bin/xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
    # qsort does not specify the order of equal keys. Undead's generator
    # depends on that order; compare the same libc sorting policy on both
    # machines, keeping every upstream engine unmodified.
    qsort=ROOT/'ports/fuzix/upstream/Library/libs/qsort.c'
    hostcmd=[os.environ.get('CC','/usr/bin/cc' if sys.platform=='darwin' else 'cc'),*sdk,'-std=c99','-D_POSIX_C_SOURCE=200809L','-DNO_TGMATH_H=1','-DCOMBINED=1','-I',str(PORT/'upstream'),'-I',str(generated),str(PORT/'tests/collectioncheck.c'),str(PORT/'platform.c'),str(qsort),*[str(PORT/'upstream'/f'{n}.c') for n in names],'-lm','-o',str(host)]
    key=hashlib.sha256(str(hostcmd).encode()+qsort.read_bytes()+b''.join((PORT/'upstream'/f'{n}.c').read_bytes() for n in names)+(PORT/'tests/collectioncheck.c').read_bytes()).hexdigest()
    stamp=output/'host.sha256'
    if not host.exists() or not stamp.exists() or stamp.read_text()!=key:
        subprocess.run(hostcmd,check=True);stamp.write_text(key)
    flags=['--target','native','--bank-size','64000','-DRIPPLE_MMU=1','-DNO_TGMATH_H=1','-DCOMBINED=1']
    for p in ['ports/fuzix/ripple/include','ports/fuzix/upstream/Library/include','runtime/include','ports/fuzix/ripple','fuse-x/build/puzzles/src']:flags+=['-I',str(ROOT/p)]
    def compile_unit(n):
        source=generated/f'{n}.c';key=hashlib.sha256(str(flags).encode()+source.read_bytes()+b''.join(h.read_bytes() for h in sorted(generated.glob('*.h')))+str((ROOT/'target/release/rcc').stat().st_mtime_ns).encode()).hexdigest()[:24]
        ir=output/f'{n}-{key}.ir'
        if not ir.exists():
            r=subprocess.run([str(ROOT/'target/release/rcc'),'compile',str(source),*flags,'--emit-ir'],capture_output=True)
            if r.returncode:raise RuntimeError(n+'\n'+r.stderr.decode(errors='replace'))
            ir.write_bytes(r.stdout)
        return ir
    with ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile_unit,names))
    temporary_exe=exe.with_name(f'.collectioncheck-{os.getpid()}.fxe')
    command=[sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'tests/collectioncheck.c'),str(PORT/'platform.c'),'-I',str(generated),'-D','NO_TGMATH_H=1','--float-format','--static','-o',str(temporary_exe)]
    for obj in objects:command+=['--object-ir',str(obj)]
    if not a.reuse_probe:
        try:subprocess.run(command,check=True);os.replace(temporary_exe,exe)
        finally:
            if temporary_exe.exists():temporary_exe.unlink()
    with tempfile.TemporaryDirectory(prefix='puzzles-collection-') as directory:
        g=Guest(Path(directory),'native',{'collectioncheck':exe},blocks=262144)
        try:
            mismatches=[]
            for i,game in enumerate(GAMES):
                name=game['id']
                if a.only and name not in a.only:continue
                case='#'+a.seed
                expected=subprocess.check_output([str(host),str(i),case],text=True,timeout=180)
                result=g.shell(f'collectioncheck {i} {shlex.quote(case)}',seconds=180)
                observed='\n'.join(l for l in result.splitlines() if re.match(r'^(BOARD|DRAW|MOVES|SOLVE|FAIL|PUZZLES_COLLECTION)\b',l))+'\n'
                (results/f'{name}-host.txt').write_text(expected);(results/f'{name}-guest.txt').write_text(observed)
                if observed!=expected or 'failures=0' not in observed:
                    mismatches.append(name);print(name,'MISMATCH',repr(observed),repr(expected),flush=True)
                else:print(name,'upstream-identical generation, drawing and input; saved history passed',flush=True)
            assert not mismatches,mismatches
        except BaseException:
            os.write(g.master,b'\x03')
            try:g.read(g.prompt,20)
            except AssertionError:pass
            raise
        finally:(results/'guest.log').write_bytes(g.log);g.close()


if __name__=='__main__':main()
