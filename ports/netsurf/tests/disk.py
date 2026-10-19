#!/usr/bin/env python3
"""Persistent HTTP cache: real process exits and guest OS reboots on private disks."""
import argparse
from email.utils import formatdate
from pathlib import Path
import sys
import tempfile
from cache import Services as CacheServices
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make
class Services(CacheServices):
    def __init__(self,path,fixtures):
        super().__init__(path,fixtures)
        self.body=b'cached'
        self.content_type=b'text/plain\r\nCache-Control: max-age=86400\r\nDate: '+formatdate(usegmt=True).encode()
    def http(self,stream):
        data=bytes(stream.received)
        if getattr(stream,'answered',False) or b'\r\n\r\n' not in data:return
        path=data.split(b' ')[1].decode()
        if not path.startswith('/disk/'):return super().http(stream)
        stream.answered=True;self.requests.append(data);self.paths.append(path);stream.path=path
        code=b'200 OK';body=b'cached'
        headers=b'Date: '+formatdate(usegmt=True).encode()+b'\r\n'
        if path.endswith('/large'):
            body=b'Z'*70000;headers+=b'Cache-Control: max-age=86400\r\n'
        # Both max-age and remaining lifetime exceed signed C16 int. The
        # explicit Age also catches a max-age silently narrowed modulo 65536.
        elif path.endswith('/fresh'):headers+=b'Cache-Control: max-age=86400\r\nAge: 25000\r\n'
        elif path.endswith('/encoding'):headers+=b'Cache-Control: max-age=86400\r\nVary: Accept-Encoding\r\n'
        elif path.endswith('/flip'):
            if self.paths.count(path)==1:headers+=b'Cache-Control: max-age=0\r\nETag: "old-copy"\r\n'
            else:headers+=b'Cache-Control: no-store\r\n'
        elif path.endswith('/validate'):
            headers+=b'Cache-Control: max-age=0\r\nETag: "disk-test"\r\n'
            if b'If-None-Match: "disk-test"\r\n' in data:code=b'304 Not Modified';body=b''
        elif path.endswith('/nocache'):
            headers+=b'Cache-Control: no-cache\r\nETag: "disk-test"\r\n'
            if b'If-None-Match: "disk-test"\r\n' in data:code=b'304 Not Modified';body=b''
        elif path.endswith('/vary'):headers+=b'Cache-Control: max-age=86400\r\nVary: Accept-Language\r\n'
        elif path.endswith('/session'):headers+=b'Cache-Control: max-age=86400\r\nSet-Cookie: test=1; Path=/session-only\r\n'
        else:headers+=b'Cache-Control: no-store\r\nETag: "must-not-store"\r\n'
        stream.queue(b'HTTP/1.1 '+code+b'\r\nContent-Type: text/plain\r\n'+headers+b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=['native','ripple'],default='native');a=p.parse_args()
    suffix='.fxe' if a.target=='native' else '.rxe'
    with tempfile.TemporaryDirectory(prefix='netsurf-disk-cache-') as name:
        work=Path(name);certs=make(work/'certs');peer=Services(work/'nic',certs)
        apps={n:ROOT/'build/netsurf'/(n+suffix) for n in ['diskcachecheck','htget']}
        files=[dict(name='/etc/ssl/cert.pem',mode=0o100644,path=str(certs/'ca.pem'))]
        guest=Guest(work,a.target,apps,files,peer,blocks=65535)
        logs=[]
        def check(args):
            result=guest.shell('diskcachecheck '+args+'; echo DISK_STATUS:$?',300)
            print(result,flush=True)
            assert 'NETSURF_DISK checks=' in result and 'failures=0' in result and 'DISK_STATUS:0' in result,result
        paths=['fresh','validate','nocache','nostore','vary','session','flip']
        urls=['http://tls.test/disk/large','http://tls.test/disk/encoding']+['http://tls.test/disk/'+path for path in paths]+['https://tls.test/binary']
        groups=[' '.join(urls[:4]),' '.join(urls[4:])] # Guest shell input is bounded to 255 cells.
        try:
            check('--store')
            for group in groups:check(group)
            for group in groups:check(group)
            counts={path:peer.paths.count('/disk/'+path) for path in paths}
            assert counts==dict(fresh=1,validate=2,nocache=2,nostore=2,vary=2,session=2,flip=2),counts
            assert peer.paths.count('/binary')==1 and peer.paths.count('/disk/large')==1,peer.paths
            assert peer.paths.count('/disk/encoding')==1,peer.paths
            # Restart the kernel on exactly the same filesystem and cache files.
            logs.append(bytes(guest.log));guest.close();guest=None
            # A reboot creates a new Ethernet attachment. Keep HTTP observations
            # but recreate the fixture's connection/MAC state for the new NIC.
            observed_paths,observed_requests=peer.paths,peer.requests
            peer.close();(work/'nic').unlink(missing_ok=True)
            peer=Services(work/'nic',certs)
            peer.paths=observed_paths;peer.requests=observed_requests
            guest=Guest(work,a.target,apps,files,peer,blocks=65535,reuse=True)
            for group in groups:check(group)
            assert peer.paths.count('/disk/fresh')==1,peer.paths
            assert peer.paths.count('/binary')==1 and peer.paths.count('/disk/large')==1,peer.paths
            assert peer.paths.count('/disk/encoding')==1,peer.paths
            assert all(peer.paths.count('/disk/'+path)==3 for path in paths[1:]),peer.paths
            assert sum(b'If-None-Match: "disk-test"' in req for req in peer.requests)==4,peer.requests
            flip=[req for req in peer.requests if b"/disk/flip " in req]
            assert b'If-None-Match: "old-copy"' in flip[1] and b'If-None-Match' not in flip[2],flip
            check('--force http://tls.test/disk/fresh')
            assert peer.paths.count('/disk/fresh')==2,peer.paths
            check('--error https://wrong.test/binary')
            check('--clear http://tls.test/disk/fresh')
            assert peer.paths.count('/disk/fresh')==3,peer.paths
            print(a.target+': persistent HTTP/verified HTTPS, ETag/304, no-store, no-cache, Vary, cookies, reload, clear and OS reboot passed')
        finally:
            if guest:logs.append(bytes(guest.log))
            (ROOT/'build/netsurf'/('disk-'+a.target+'.log')).write_bytes(b'\nREBOOT\n'.join(logs))
            if guest:guest.close()
            peer.close()
if __name__=='__main__':main()
