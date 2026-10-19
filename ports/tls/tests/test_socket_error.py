#!/usr/bin/env python3
"""Preserve a real TCP reset behind wolfSSL's generic SOCKET_ERROR_E."""
import argparse,tempfile
from pathlib import Path
from guest import Guest,ROOT
from fixtures import make
from peer import Services

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['native','ripple'],default='native')
    p.add_argument('--executable',type=Path,required=True)
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='tls-reset-') as name:
        work=Path(name);certs=make(work/'certs');peer=Services(work/'nic',certs)
        def reset(stream):
            stream.send(stream.next,20)
            stream.reset=stream.closed=True
        peer.tcp_services[453]=reset
        g=Guest(work,a.target,{'htget':a.executable},
                [dict(name='/etc/ssl/cert.pem',path=str(certs/'ca.pem'))],peer)
        try:
            text=g.shell('RIPPLE_TLS_PROFILE=1 htget https://tls.test:453/binary /download; echo RESET_STATUS:$?',90)
            print(text,flush=True)
            assert 'RESET_STATUS:1' in text,text
            assert 'Connection reset by peer (TLS error -308)' in text,text
            assert 'TLS failed after ' in text and 'TLS crypto accelerator: enabled' in text,text
        finally:g.close();peer.close()
    print(a.target+': TCP reset retains socket errno, TLS code, failure timing and accelerator state')
if __name__=='__main__':main()
