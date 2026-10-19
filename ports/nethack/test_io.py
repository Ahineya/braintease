#!/usr/bin/env python3
"""C16 binary stdio regression on real disposable guest filesystems."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import os
import time

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def run(args, **kwargs):
    return subprocess.run(list(map(str,args)),check=True,cwd=ROOT,**kwargs)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target',choices=['native','ripple'])
    group=parser.add_mutually_exclusive_group()
    group.add_argument('--packed',action='store_true',help='test signed packed level operands instead of stdio')
    group.add_argument('--keys',action='store_true',help='exercise real terminal arrow sequences and Escape')
    args=parser.parse_args()
    build=PORT/('build/keys-test' if args.keys else 'build/io-test');build.mkdir(parents=True,exist_ok=True)
    source=PORT/('tests/keys.c' if args.keys else 'tests/packed.c' if args.packed else 'tests/c16_io.c')
    if args.target=='native':
        binary=build/'c16io.fxe'
        run(['python3',ROOT/'fuse-x/cc.py',source,'--static','-o',binary])
    else:
        binary=build/'c16io.rxe';sdk=ROOT/'build/fuzix';fuzix=ROOT/'ports/fuzix'
        includes=[fuzix/'ripple/include',fuzix/'upstream/Library/include',ROOT/'runtime/include',fuzix/'ripple']
        run([ROOT/'target/release/rcc','compile',source,'--bank-size','64000','-DRIPPLE_MMU=1','-DRIPPLE=1',*[f'-I{p}' for p in includes],'-o',build/'c16io.asm'])
        run([ROOT/'src/ripple-asm/target/release/rasm','assemble',build/'c16io.asm','-o',build/'c16io.pobj','--bank-size','64000','--max-immediate','65535'])
        run(['make','-C',fuzix,f'BUILD={sdk}','rfl-sdk'])
        info=json.loads(run(['make','-s','-C',fuzix,f'BUILD={sdk}','rfl-sdk-info'],capture_output=True,text=True).stdout)
        run([ROOT/'src/ripple-asm/target/release/rlink',info['entry'],build/'c16io.pobj',*info['libraries'],'-f','rxe','--bank-size','64000','-o',binary])
    directory=Path(tempfile.mkdtemp(prefix='nethack-stdio-'))
    guest=Guest(directory,args.target,{'c16io':binary})
    active=False
    try:
        if args.keys:
            for mode,expected in enumerate(['kjlh','8264','2864','\x1b'*4]):
                os.write(guest.master,('c16io '+str(mode)+'\r').encode())
                active=True
                guest.read(b'KEYS READY')
                index=0
                def expect(values):
                    nonlocal index
                    for value in values:
                        guest.read(('KEY %d %d'%(index,ord(value))).encode(),15)
                        index+=1
                # Burst input exercises stdio lookahead as well as descriptor reads.
                os.write(guest.master,b'a\x1b[A\x1b[B\x1b[C\x1b[D\x1bOA\x1bOB\x1bOC\x1bOD')
                expect('a'+expected*2)
                os.write(guest.master,b'\x1b');expect('\x1b')
                os.write(guest.master,b'\x1bz');expect('\x1bz')
                os.write(guest.master,b'\x1b[')
                time.sleep(.03)
                os.write(guest.master,b'A');expect(expected[0])
                os.write(guest.master,b'!')
                print(guest.read(guest.prompt),flush=True)
                active=False
            print('TERMINAL KEYS PASS '+args.target,flush=True)
            return
        output=guest.shell('c16io');print(output,flush=True)
        assert ('Packed level operands PASS' if args.packed else 'C16 binary stdio PASS') in output,output
    finally:
        try:
            if active:
                os.write(guest.master,b'!')
                guest.read(guest.prompt,10)
        finally: guest.close()

if __name__=='__main__': main()
