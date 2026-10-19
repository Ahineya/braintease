#!/usr/bin/env python3
"""Real guest cache reuse, ETag/304 revalidation and no-store fetch counts."""
import argparse,sys,tempfile
from pathlib import Path
from email.utils import formatdate
from web import Services as WebServices
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make
class Services(WebServices):
    def http(self,stream):
        data=bytes(stream.received)
        if getattr(stream,'answered',False) or b'\r\n\r\n' not in data:return
        path=data.split(b' ')[1].decode()
        if path in ('/progress-known','/progress-unknown'):
            stream.answered=True;self.paths.append(path);stream.path=path;stream.chunk=127
            body=b'<title>Progress</title><!--'+b'x'*24000+b'--><p>Complete</p>'
            header=b'Content-Length: '+str(len(body)).encode()+b'\r\n' if path.endswith('known') and not path.endswith('unknown') else b''
            stream.queue(b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\n'+header+b'\r\n'+body);return
        if not path.startswith('/cache/'):return super().http(stream)
        stream.answered=True;self.requests.append(data);self.paths.append(path);stream.path=path
        headers=b'Date: '+formatdate(usegmt=True).encode()+b'\r\n'
        code=b'200 OK';body=b'cached'
        if path.endswith('/fresh'):headers+=b'Cache-Control: max-age=3600\r\n'
        elif path.endswith('/validate'):
            headers+=b'Cache-Control: max-age=0\r\nETag: "cache-test"\r\n'
            if b'If-None-Match: "cache-test"\r\n' in data:code=b'304 Not Modified';body=b''
        else:headers+=b'Cache-Control: no-store\r\n'
        stream.queue(b'HTTP/1.1 '+code+b'\r\nContent-Type: text/plain\r\n'+headers+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['native','ripple'],default='native');a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='netsurf-cache-') as name:
        work=Path(name);peer=Services(work/'nic',make(work/'certs'));suffix='.fxe' if a.target=='native' else '.rxe'
        g=Guest(work,a.target,{n:ROOT/'build/netsurf'/(n+suffix) for n in ['cachecheck','htget']},peer=peer,blocks=65535)
        try:
            result=g.shell('cachecheck; echo CACHE_STATUS:$?',180);print(result,flush=True)
            assert 'failures=0' in result and 'CACHE_STATUS:0' in result,result
            counts={name:peer.paths.count('/cache/'+name) for name in ['fresh','validate','private']}
            assert counts==dict(fresh=1,validate=2,private=2),counts
            assert any(b'If-None-Match: "cache-test"' in req for req in peer.requests),peer.requests
            print(a.target+': fresh response fetched once, ETag 304 reused body, no-store fetched twice')
        finally:
            (ROOT/'build/netsurf'/('cache-'+a.target+'.log')).write_bytes(g.log);g.close();peer.close()
if __name__=='__main__':main()
