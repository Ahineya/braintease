#!/usr/bin/env python3
"""Opt-in public browsing with guest HTTP/TLS, using a blind TCP relay.

Public sites change: this supplements the deterministic web.py acceptance test.
The relay neither decrypts TLS nor substitutes certificates or page content.
"""
import argparse
from pathlib import Path
import sys
import tempfile
from public_peer import Relay, ROOT
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],default='native')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    parser.add_argument('--url',action='append',help='Override the default public-site checks')
    args=parser.parse_args();args.build=args.build.resolve()
    suffix='.fxe' if args.target=='native' else '.rxe'
    cases=[(u,None) for u in args.url] if args.url else [
        ('http://wiby.me/','Wiby'),('https://wiby.me/','Wiby'),
        ('https://example.com/','Example Domain'),('https://wiby.me/surprise/',None)]
    results=[]
    with tempfile.TemporaryDirectory(prefix='netsurf-internet-') as temporary:
        work=Path(temporary);peer=Relay(work/'nic')
        files=[dict(name='/etc/ssl/cert.pem',path=str(ROOT/'ports/tls/roots/cert.pem'))]
        g=Guest(work,args.target,{n:args.build/(n+suffix) for n in ['htget','networkcheck']},files,peer,blocks=65535)
        try:
            for url,expected in cases:
                # CLI arguments are test input, never shell interpolation from a page.
                import shlex
                command='networkcheck '+shlex.quote(url)
                if expected:command+=' '+shlex.quote(expected)
                text=g.shell(command+'; echo PUBLIC_STATUS:$?',400)
                print(text,flush=True);results.append(text)
                assert 'failures=0' in text and 'PUBLIC_STATUS:0' in text,text
                assert 'NETWORK_PAGE title=' in text,text
                import re
                final=re.search(r'NETWORK_PAGE title=.*? url=(\S+)',text).group(1)
                assert final.startswith(('http://','https://')),text
                assert re.search(r'NETWORK_PAGE .* text=\S',text),text
                if '/surprise/' in url:
                    assert final!=url and 'wiby.me/surprise' not in final,final
            assert not peer.failures,peer.failures
        finally:
            (args.build/('internet-'+args.target+'.log')).write_bytes(g.log)
            g.close();peer.close()
        (args.build/('internet-'+args.target+'-results.txt')).write_text('\n'.join(results))
    print(args.target+': public HTTP/HTTPS browser rendering and clean filesystem passed')

if __name__=='__main__':main()
