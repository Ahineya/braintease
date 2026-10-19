#!/usr/bin/env python3
"""Document-local image sharing against actual no-store HTTP responses."""
import argparse
from pathlib import Path
import sys
import tempfile
from bench_fetch import Services
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=('native','ripple'),default='native')
    p.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    args=p.parse_args();suffix='.fxe' if args.target=='native' else '.rxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-shared-',dir='/tmp') as name:
        work=Path(name);peer=Services(work/'nic',make(work/'cert'),no_store=True);guest=None
        peer.assets['/next.html']=peer.assets['/page.html']
        try:
            guest=Guest(work,args.target,{n:args.build.resolve()/(n+suffix) for n in ('sharedcheck','htget')},peer=peer,blocks=65535)
            result=guest.shell('sharedcheck; echo SHARED_STATUS:$?',180);print(result,flush=True)
            assert 'failures=0' in result and 'SHARED_STATUS:0' in result,result
            assert peer.paths.count('/image-0.png')==4 and peer.paths.count('/image-1.png')==3,peer.paths
            assert all(peer.paths.count('/image-'+str(i)+'.png')==2 for i in range(2,8)),peer.paths
            assert peer.paths.count('/page.html')==peer.paths.count('/next.html')==1,peer.paths
            assert len(peer.paths)==21,peer.paths
        finally:
            try:
                if guest:
                    (ROOT/'build/netsurf'/('shared-'+args.target+'.log')).write_bytes(guest.log)
                    guest.close()
            finally:peer.close()
    print(args.target+': pending and completed references share; cancellation, independent retrieval and URL isolation passed')

if __name__=='__main__':main()
