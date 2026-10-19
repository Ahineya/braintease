#!/usr/bin/env python3
"""Verify anchor reuse without stale trust, skipped hostnames or forked sessions."""
import argparse
from pathlib import Path
import tempfile
from fixtures import make
from guest import Guest,ROOT
from peer import Services


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=('native','ripple'),default='native')
    p.add_argument('--executable',type=Path,required=True)
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='tls-trust-cache-',dir='/tmp') as temporary:
        d=Path(temporary);certs=make(d/'certs');peer=Services(d/'nic',certs)
        # Deliberately equal lengths: size/mtime-only cache keys are insufficient.
        for source,name in [('ca.pem','good'),('other.pem','other')]:
            data=(certs/source).read_bytes();assert len(data)<4096
            (d/name).write_bytes(data+b'\n'*(4096-len(data)))
        files=[dict(name='/'+n,path=str(d/n)) for n in ['good','other']]
        g=None
        try:
            g=Guest(d,a.target,{'trustreuse':a.executable},files,peer,blocks=65536)
            text=g.shell('RIPPLE_TLS_PROFILE=1 trustreuse; echo CACHE_STATUS:$?',600)
            print(text,flush=True)
            assert 'trustreuse passed' in text and 'CACHE_STATUS:0' in text,text
            assert '(reused)' in text.split('CACHE repeat',1)[1].split('CACHE hostname',1)[0],text
            assert '(reused)' not in text.split('CACHE replaced same size',1)[1].split('CACHE restored',1)[0],text
            child=text.split('CACHE fork',1)[1].split('CACHE child fresh',1)[0]
            # First line opens the parent's live session (reuse), then child rebuilds.
            assert child.count('TLS trust detail:')==2 and child.count('(reused)')==1,text
        finally:
            if g:g.close()
            peer.close()
    print(a.target+': trust reuse, equal-size replacement, missing files, hostname rejection, live references and fork passed')


if __name__=='__main__':main()
