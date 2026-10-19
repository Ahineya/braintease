#!/usr/bin/env python3
"""Persistent guest HTTP/TLS worker: framing, keep-alive and recovery."""
import argparse
import json
from pathlib import Path
import ssl
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make
from peer import Services as TLSService


class Services(TLSService):
    def response(self,stream,request,tls=False):
        path=request.split(b' ')[1].decode().lstrip('/')
        self.paths.append(('tls:' if tls else 'http:')+path)
        self.requests.append(request)
        if not getattr(stream,'requests',0):
            if not hasattr(self,'connections'):self.connections=[]
            self.connections.append(stream)
        stream.requests=getattr(stream,'requests',0)+1
        stream.path='/slow' # HTTPServices must not auto-close a persistent peer.
        if path=='one' and not tls:assert b'Cookie: first=yes\r\n' in request
        if path=='two':assert b'Cookie: ' not in request
        if path=='long':assert request.count(b'X-A: ')==4 and len(request)>4096
        prefix=b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n'
        if path=='binary':return prefix+b'Content-Length: 5120\r\n\r\n'+bytes(range(256))*20
        if path=='empty':return b'HTTP/1.1 204 No Content\r\n\r\n'
        if path=='notmodified':return b'HTTP/1.1 304 Not Modified\r\nContent-Length: 999999\r\n\r\n'
        if path=='close':return prefix+b'Connection: keep-alive, Close\r\nContent-Length: 5\r\n\r\nCLOSE'
        if path=='old':return b'HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nOLD'
        if path=='partial':return b'HTTP/1.1 '
        if path=='bad':
            return prefix+b'Content-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nxx'
        if path=='redirect':return b'HTTP/1.1 302 Found\r\nLocation: /after\r\nContent-Length: 9\r\n\r\nunconsumed'
        if path=='chunked':
            body=b'TLS-CHUNKED' if tls else b'ABCDEF'
            return prefix+b'Transfer-Encoding: chunked\r\n\r\n'+f'{len(body):x}'.encode()+b'\r\n'+body+b'\r\n0\r\nX-Trailer: done\r\n\r\n'
        body={'one':b'ONE','two':b'TWO','after':b'AFTER','stale':b'STALE','retry':b'RETRY','after-bad':b'RECOVERED','long':b'LONG'}[path]
        if tls:body=b'TLS-'+body
        return prefix+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body

    def http(self,stream):
        if stream.closing:return
        at=getattr(stream,'request_at',0)
        request=bytes(stream.received[at:])
        if b'\r\n\r\n' not in request:return
        end=request.index(b'\r\n\r\n')+4;request=request[:end]
        stream.request_at=at+end
        stream.queue(self.response(stream,request))
        if request.startswith((b'GET /stale ',b'GET /partial ')):stream.closing=True

    def tls(self,stream):
        if stream.closing:return
        if not hasattr(stream,'tls'):
            stream.incoming=ssl.MemoryBIO();stream.outgoing=ssl.MemoryBIO()
            stream.tls=self.contexts[stream.local].wrap_bio(stream.incoming,stream.outgoing,server_side=True)
            stream.tls_at=0;stream.tls_ready=False;stream.request=bytearray()
        fresh=bytes(stream.received[stream.tls_at:]);stream.tls_at=len(stream.received)
        if fresh:stream.incoming.write(fresh)
        try:
            if not stream.tls_ready:
                stream.tls.do_handshake();stream.tls_ready=True;self.versions.append(stream.tls.version())
            while True:
                part=stream.tls.read(16384)
                if not part:stream.closing=True;break
                stream.request.extend(part)
                if b'\r\n\r\n' not in stream.request:continue
                end=stream.request.index(b'\r\n\r\n')+4
                request=bytes(stream.request[:end]);del stream.request[:end]
                stream.tls.write(self.response(stream,request,True))
                if request.startswith(b'GET /stale '):
                    try:stream.tls.unwrap()
                    except ssl.SSLWantReadError:pass
                    stream.closing=True;break
        except (ssl.SSLWantReadError,ssl.SSLZeroReturnError):pass
        except ssl.SSLError as error:self.failures.append(str(error));stream.closing=True
        finally:
            while stream.outgoing.pending:stream.queue(stream.outgoing.read())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=('native','ripple'),default='native')
    parser.add_argument('--browser',action='store_true',help='exercise the NetSurf adapter as well as the worker')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    args=parser.parse_args();suffix='.fxe' if args.target=='native' else '.rxe'
    build=args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='http-worker-',dir='/tmp') as folder:
        work=Path(folder);cert=make(work/'cert');peer=Services(work/'nic',cert)
        guest=Guest(work,args.target,{name:build/(name+suffix) for name in ('htget','reusecheck' if args.browser else 'httpworker')},
                    [dict(name='/etc/ssl/cert.pem',path=str(cert/'ca.pem'))],peer,blocks=65535)
        try:
            text=guest.shell(('reusecheck' if args.browser else 'httpworker')+'; echo WORKER_STATUS:$?',400);print(text,flush=True)
            assert 'failures=0' in text and 'WORKER_STATUS:0' in text,text
            # First three plaintext and HTTPS requests share their respective
            # TCP sockets; stale idle sockets each cause exactly one new dial.
            streams=peer.connections
            assert any(s.local==80 and s.requests>=3 for s in streams)
            assert any(s.local==443 and s.requests>=4 for s in streams)
            assert peer.paths.count('http:retry')==1 and peer.paths.count('tls:retry')==1,peer.paths
            assert len(peer.versions)==(4 if args.browser else 3),peer.versions
            if not args.browser:
                assert peer.versions[-1]=='TLSv1.2'
                assert any(s.local==444 and s.requests==2 for s in streams)
            result=dict(paths=peer.paths,handshakes=peer.versions,connections=[dict(port=s.local,requests=s.requests) for s in streams])
            (build/(('worker-browser-' if args.browser else 'worker-')+args.target+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        finally:
            (build/(('worker-browser-' if args.browser else 'worker-')+args.target+'.log')).write_bytes(guest.log)
            try:guest.close()
            finally:peer.close()
    print(args.target+': persistent HTTP/TLS framing, reuse, stale retry and malformed IPC passed')

if __name__=='__main__':main()
