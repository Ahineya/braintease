#!/usr/bin/env python3
"""Repeatable NetSurf HTTP/HTTPS checks through real guest Ethernet/TCP/TLS."""
import argparse
import os
from pathlib import Path
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make
from peer import Services as TLSService
PAGE=b'''<!doctype html><title>Verified guest web</title><style>body {background:#223344;color:white}</style><h1>Real guest HTTPS</h1><p>Certificate checked before rendering.</p>'''
HOME=b'''<!doctype html><title>Network CSS page</title><link rel="stylesheet" href="/site.css"><h1>HTTP and external CSS</h1><p><a href="/redirect">Go onward</a></p><form method="get" action="/search"><input name="q"></form>'''
class Services(TLSService):
    def __init__(self,path,fixtures):
        super().__init__(path,fixtures,PAGE,b'text/html; charset=utf-8')
    def http(self,stream):
        data=bytes(stream.received)
        if getattr(stream,'answered',False) or b'\r\n\r\n' not in data:return
        stream.answered=True
        self.requests.append(data)
        method,path,version=data.split(b'\r\n',1)[0].decode().split()
        assert method=='GET'
        self.paths.append(path);stream.path=path
        code=b'200 OK';headers=b'';body=b'';mime=b'text/html'
        if path=='/':body=HOME;headers=b'Set-Cookie: sample=abc; Path=/\r\n'
        elif path=='/site.css':
            assert b'Cookie: sample=abc\r\n' in data,data
            body=b'body { background: #112233; color: #eeeeee }';mime=b'text/css'
        elif path=='/next':body=b'<title>Redirect target</title><h1>Network redirect passed</h1>'
        elif path=='/redirect':code=b'302 Found';headers=b'Location: /next\r\n'
        elif path=='/meta':body=b'<meta http-equiv="refresh" content="0;url=/next"><title>Meta refresh</title>'
        elif path=='/loop':code=b'302 Found';headers=b'Location: /loop\r\n'
        elif path=='/cookie':headers=b'Set-Cookie: sample=abc; Path=/\r\n';body=b'cookie stored'
        elif path=='/cookie-check':
            assert b'Cookie: sample=abc\r\n' in data,data
            body=b'cookie returned'
        elif path.startswith('/search?'):body=b'<title>Search</title><p>Query received</p>'
        elif path=='/bad-framing' or path.startswith('/bad-framing?'):
            stream.queue(b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nxxx');return
        elif path=='/short':
            stream.queue(b'HTTP/1.1 200 OK\r\nContent-Length: 9999\r\n\r\nshort');return
        elif path=='/big':body=b'x'*262145;mime=b'text/plain'
        elif path=='/abort':
            stream.queue(b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstart');stream.path='/slow';return
        elif path=='/chunked':
            stream.queue(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n3;x=yes\r\nabc\r\n3\r\ndef\r\n0\r\nX-Trailer: done\r\n\r\n');return
        elif path=='/favicon.ico':code=b'404 Not Found'
        elif path=='/notfound':code=b'404 Not Found';body=b'<title>Not found</title><h1>Readable HTTP error page</h1>'
        else:raise AssertionError(path)
        stream.chunk=127
        stream.queue(b'HTTP/1.1 '+code+b'\r\nContent-Type: '+mime+b'\r\n'+headers+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],default='native')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    parser.add_argument('--smoke',action='store_true')
    args=parser.parse_args();suffix='.fxe' if args.target=='native' else '.rxe'
    args.build=args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='netsurf-web-') as temporary:
        work=Path(temporary);fixtures=make(work/'certificates');peer=Services(work/'nic',fixtures)
        files=[dict(name='/etc/ssl/cert.pem',mode=0o100644,path=str(fixtures/'ca.pem')),
               dict(name='/other-root',mode=0o100644,path=str(fixtures/'other.pem'))]
        g=Guest(work,args.target,{n:args.build/(n+suffix) for n in ['htget','networkcheck']},files,peer,blocks=65535)
        results=[]
        def check(command):
            text=g.shell(command+'; echo WEB_STATUS:$?',400)
            print(text,flush=True);results.append(text)
            assert 'NETSURF_NETWORK checks=' in text and 'failures=0' in text and 'WEB_STATUS:0' in text,text
        try:
            # Error/timeout pages import this stylesheet. A 32-bit colour
            # followed by %s previously desynchronized C16 printf arguments.
            check('networkcheck --raw about:nscolours.css "background-color: #e0e0e0;"')
            check('networkcheck http://tls.test/ "HTTP and external CSS"')
            assert '/site.css' in peer.paths,peer.paths
            check('networkcheck http://tls.test/redirect "Network redirect passed"')
            check('networkcheck https://tls.test/binary "Real guest HTTPS"')
            check('networkcheck --recover http://tls.test/ http://tls.test/bad-framing FetchErrorTitle')
            check('networkcheck https://wrong.test/binary FetchErrorTitle')
            if not args.smoke:
                check('networkcheck --raw http://tls.test/chunked abc')
                check('networkcheck http://tls.test/meta "Network redirect passed"')
                check('networkcheck http://tls.test/notfound "Readable HTTP error page"')
                check('networkcheck --error http://tls.test/bad-framing')
                check('networkcheck --error http://tls.test/short')
                check('networkcheck --error http://tls.test/big')
                check('networkcheck --abort http://tls.test/abort')
                check('networkcheck https://tls.test/redirect "Real guest HTTPS"')
                check('networkcheck --error https://wrong.test/binary "TLS error"')
                check('networkcheck --cacert /other-root --error https://tls.test/binary "TLS error"')
                check('networkcheck --error https://tls.test/truncated')
                check('networkcheck --error https://tls.test/unclean')
            assert 'tls.test' in peer.sni,peer.sni
        finally:
            (args.build/('web-'+args.target+'.log')).write_bytes(g.log)
            g.close();peer.close()
        (args.build/('web-'+args.target+'-results.txt')).write_text('\n'.join(results))
    print(args.target+': NetSurf guest HTTP/HTTPS and clean filesystem passed')
if __name__=='__main__':main()
