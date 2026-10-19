#!/usr/bin/env python3
"""Time a real Nano-X image page with supplied browser/downloader binaries.

Private disks, guest DNS/TCP/TLS, an independent OpenSSL peer, no host decoder.
Run baseline and candidate separately while other builds are idle.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import sys
import tempfile
import time
import zlib

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
from fixtures import make
from test_httpworker import Services as PersistentServices


def chunk(kind,data):
    return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data))


def image(width=32):
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',width,width,8,2,0,0,0))+
            chunk(b'IDAT',zlib.compress((b'\0'+bytes([110,80,190])*width)*width))+chunk(b'IEND',b''))


class Services(PersistentServices):
    def __init__(self,path,cert,local=False,no_store=False):
        super().__init__(path,cert)
        self.connections=[];self.no_store=no_store
        count=4 if local else 8;size=128 if local else 32
        self.assets={'/page.html':(b'text/html',b'<!doctype html><title>Fetch benchmark</title><p>Image fetch benchmark</p>'+b''.join(
            f'<img src="/image-{i}.png" width="64" height="64">'.encode() for i in range(count)))}
        self.assets.update({f'/image-{i}.png':(b'image/png',image(size)) for i in range(count)})

    def response(self,stream,request,tls=False):
        path=request.split(b' ')[1].decode()
        self.paths.append(path)
        if not getattr(stream,'requests',0):self.connections.append(stream)
        stream.requests=getattr(stream,'requests',0)+1
        stream.path='/slow';stream.chunk=1400
        mime,body=self.assets[path]
        return (b'HTTP/1.1 200 OK\r\nContent-Type: '+mime+b'\r\nContent-Length: '+str(len(body)).encode()+
                b'\r\nCache-Control: '+(b'no-store' if self.no_store else b'max-age=3600')+b'\r\n\r\n'+body)


class TimedGuest(Guest):
    def pump(self):
        if self.peer:self.peer.pump()
        if select.select([self.master],[],[],.001)[0]:
            try:data=os.read(self.master,65536)
            except OSError:data=b''
            if data:self.log.extend(data)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/netsurf',help='browser and htget directory')
    p.add_argument('--no-store',action='store_true',help='verify one fetch/decode per no-store image within a document')
    p.add_argument('--local',action='store_true',help='check four local 128x128 images without duplicate decodes')
    p.add_argument('--full-trust',action='store_true',help='append the pinned public roots to the fixture CA')
    p.add_argument('--repeat',type=int,default=3)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.build=args.build.resolve();args.output=args.output.resolve()
    rows=[];old=dict(os.environ)
    os.environ['NANOX_KEEP_TTY']='1'
    for name in ('FUSE_X_FRAMES','FUSE_X_TEST_EVENT'):os.environ.pop(name,None)
    with tempfile.TemporaryDirectory(prefix='netsurf-bench-',dir='/tmp') as folder:
        work=Path(folder);cert=make(work/'cert');peer=Services(work/'nic',cert,args.local,args.no_store);peer.sock.settimeout(.001)
        trust=cert/'ca.pem'
        if args.full_trust:
            trust=work/'full.pem';trust.write_bytes((ROOT/'ports/tls/roots/cert.pem').read_bytes()+(cert/'ca.pem').read_bytes())
        apps={n:args.build/(n+'.fxe') for n in ('netsurf','htget')}
        apps.update({n:ROOT/'fuse-x/build/apps'/(n+'.fxe') for n in ('nano-X','nxhostcheck','kill')})
        files=[dict(name='/dev/gfx',mode=0o20600,device=0x400),dict(name='/etc/ssl/cert.pem',path=str(trust))]
        files.append(dict(name='/usr/share/netsurf/web.nfa',path=str(ROOT/'ports/netsurf/fonts/web.nfa')))
        if args.local:
            for name,(_,body) in peer.assets.items():
                path=work/name[1:];path.write_bytes(body);files.append(dict(name=name,path=str(path)))
        measured=dict(apps,kernel=Path(os.environ.get('FUSE_X_TEST_KERNEL',str(ROOT/'fuse-x/build/fuse-x'))))
        metadata={n:dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for n,path in measured.items()}
        # Pin the exact executable bytes for the whole run even if another
        # build atomically replaces the original artifacts in the meantime.
        for name in ('netsurf','htget'):
            data=apps[name].read_bytes();assert hashlib.sha256(data).hexdigest()==metadata[name]['sha256']
            apps[name]=work/(name+'.fxe');apps[name].write_bytes(data)
        guest=None
        try:
            guest=TimedGuest(work,'native',apps,files,peer,blocks=131072)
            guest.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');guest.shell('nano-X -p &');guest.shell('s=$!')
            if b'Nano-X ready' not in guest.log:guest.read(b'Nano-X ready')
            for repetition in range(args.repeat):
                for scheme in (('file',) if args.local else ('http','https')):
                    before=len(guest.log);connections=len(peer.connections);handshakes=len(peer.versions);requests=len(peer.paths)
                    start=time.monotonic()
                    url='/page.html' if args.local else scheme+'://tls.test/page.html'
                    guest.shell('NETSURF_TRACE_IMAGES=1 netsurf --no-disk-cache --images --trace '+url+' &')
                    guest.shell('p=$!')
                    if b'NETSURF_GUI_DOCUMENT ' not in guest.log[before:]:guest.read(b'NETSURF_GUI_DOCUMENT ',180)
                    elapsed=time.monotonic()-start
                    log=bytes(guest.log[before:])
                    decoded=4 if args.local else 8;requested=0 if args.local else 9
                    events=re.findall(rb'NETSURF_IMAGE (\S+) .*status=0',log)
                    assert len(set(events))==decoded and b'NETSURF_IMAGE_REJECT' not in log,log
                    actual_requests=len(peer.paths)-requests
                    assert len(events)==decoded and actual_requests==requested,(log,peer.paths[requests:])
                    row=dict(scheme=scheme,repetition=repetition,seconds=round(elapsed,4),connections=len(peer.connections)-connections,
                             handshakes=len(peer.versions)-handshakes,requests=actual_requests,decoded=len(events),unique_decodes=len(set(events)))
                    print(json.dumps(row),flush=True);rows.append(row)
                    wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',log)[-1]);guest.shell(f'nxhostcheck ctl close {wid}')
                    assert 'BROWSER_STATUS:0' in guest.shell('wait $p; echo BROWSER_STATUS:$?')
            guest.shell('kill $s; wait $s')
        finally:
            try:
                if guest:guest.close()
            finally:
                peer.close();os.environ.clear();os.environ.update(old)
                args.output.parent.mkdir(parents=True,exist_ok=True)
                args.output.write_text(json.dumps(dict(full_trust=args.full_trust,no_store=args.no_store,binaries=metadata,samples=rows),indent=2)+'\n')
                if guest:args.output.with_suffix('.log').write_bytes(guest.log)
    print('Image fetch benchmark and clean filesystem passed')


if __name__=='__main__':main()
