#!/usr/bin/env python3
"""Validate local file ETags and retained source reuse on a private root."""
import argparse
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=('native','ripple'),default='native');args=p.parse_args()
    suffix='.fxe' if args.target=='native' else '.rxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-filecache-',dir='/tmp') as name:
        work=Path(name);g=Guest(work,args.target,{'filecachecheck':ROOT/'build/netsurf'/('filecachecheck'+suffix)},
                              [dict(name='/fixture.txt',text='cached')],blocks=65535)
        try:
            text=g.shell('filecachecheck; echo FILECACHE_STATUS:$?');print(text,flush=True)
            assert 'failures=0' in text and 'FILECACHE_STATUS:0' in text,text
        finally:
            (ROOT/'build/netsurf'/('filecache-'+args.target+'.log')).write_bytes(g.log);g.close()
    print(args.target+': local validator and source reuse passed')
if __name__=='__main__':main()
