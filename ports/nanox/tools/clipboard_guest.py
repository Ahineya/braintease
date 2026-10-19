#!/usr/bin/env python3
"""Real Nano-X clipboard protocol on disposable native and RVM/JIT guests."""
import argparse,os,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['native','ripple','both'],default='both');a=p.parse_args()
for target in (('native','ripple') if a.target=='both' else (a.target,)):
    native=target=='native';build=ROOT/('fuse-x/build' if native else 'build/fuzix');suffix='.fxe' if native else '.rxe'
    apps={n:build/('apps/'+n+suffix if native else n+suffix) for n in ('nano-X','nxcheck','kill')}
    with tempfile.TemporaryDirectory(prefix='nanox-clipboard-') as folder:
        work=Path(folder);saved=dict(os.environ);os.environ['NANOX_KEEP_TTY']='1'
        try:g=Guest(work,target,apps,[dict(name='/dev/gfx',mode=0o20600,device=0x400)])
        finally:os.environ.clear();os.environ.update(saved)
        try:
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('server=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            out=g.shell('nxcheck --clipboard; echo CLIP_STATUS:$?',180)
            assert 'NANOX_CLIPBOARD_OK' in out and 'CLIP_STATUS:0' in out,out
            g.shell('kill $server; wait $server')
        finally:
            g.close();(build/('clipboard-protocol-'+target+'.log')).write_bytes(g.log)
    print(target+': 32 KiB clipboard roundtrip, split UTF-8, ownership, oversize and empty commit passed')
