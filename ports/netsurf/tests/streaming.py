#!/usr/bin/env python3
"""Image prefix refusal, parallel fetches and scrolling while resources load."""
import argparse
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/lynx'))
from http_peer import Services as HttpServices
sys.path.insert(0,str(ROOT/'libraries/tests'))
from image_fixtures import chunk,zlib

PNG=(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',4,2,8,6,0,0,0))+
     chunk(b'IDAT',zlib.compress((b'\0'+bytes([255,0,0,255])*4)*2))+chunk(b'IEND',b''))
PAGE=b'''<!doctype html><title>Streaming images</title><style>
body{margin:0;color:#eeeeee;background:#204060}div{height:1800px}
</style><h1>Text before images finish</h1>
<img src="/too-wide.png"><img src="/unsupported.png"><img src="/huge">
<img src="/delayed.png" width="128" height="64"><div>Long page</div>
<p>Bottom of the page remains usable.</p>'''


class Services(HttpServices):
    def __init__(self,path):
        super().__init__(path)
        self.paths=[]
        self.delayed=[]
        self.tcp_services={80:self.http}

    def http(self,stream):
        if getattr(stream,'answered',False) or b'\r\n\r\n' not in stream.received:return
        stream.answered=True
        path=bytes(stream.received).split(b' ')[1].decode()
        self.paths.append(path)
        stream.path='/slow' # keep held bodies open until the client aborts
        mime=b'text/plain';body=b'';length=None
        if path=='/':body=PAGE;mime=b'text/html';length=len(body)
        elif path=='/held':body=b'x';length=4096
        elif path=='/fast':body=b'FAST';length=4
        elif path=='/third':body=b'THIRD';length=5
        elif path=='/huge':length=19243485;mime=b'image/gif'
        elif path=='/too-wide.png':
            mime=b'image/png'
            body=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',1248,768,8,6,0,0,0))+b'x'*512
        elif path=='/unsupported.png':mime=b'image/png';body=b'RIFF\xff\xff\0\0WEBP'+b'x'*512
        elif path=='/delayed.png':
            mime=b'image/png';length=len(PNG)
            self.delayed.append((time.monotonic()+2,stream,PNG))
        else:raise AssertionError(path)
        stream.queue(b'HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: '+mime+b'\r\n'+
                     (b'Content-Length: '+str(length).encode()+b'\r\n' if length is not None else b'')+
                     b'\r\n'+body)

    def pump(self):
        super().pump()
        for when,stream,body in self.delayed[:]:
            if time.monotonic()>=when:
                stream.queue(body)
                self.delayed.remove((when,stream,body))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple'],default='native')
    parser.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    args=parser.parse_args();build=args.build.resolve();native=args.target=='native';suffix='.fxe' if native else '.rxe'
    output=build/('streaming-'+args.target+'.log')
    with tempfile.TemporaryDirectory(prefix='netsurf-stream-',dir='/tmp') as folder:
        work=Path(folder);peer=Services(work/'nic');guest_build=ROOT/('fuse-x/build' if native else 'build/fuzix')
        apps={n:build/(n+suffix) for n in ['htget','streamcheck','scrollcheck']}
        apps.update({n:guest_build/('apps/'+n+suffix if native else n+suffix) for n in ['nano-X','kill']})
        files=[dict(name='/dev/gfx',mode=0o20600,device=0x400),
               dict(name='/usr/share/netsurf/web.nfa',path=str(ROOT/'ports/netsurf/fonts/web.nfa'))]
        old=dict(os.environ)
        os.environ['NANOX_KEEP_TTY']='1'
        # Guest's fixture uses eight-sector FAT16 clusters. Keep the outer
        # container below that cluster ceiling while fitting both large probes.
        try:g=Guest(work,args.target,apps,files,peer,blocks=114688,disk_sectors=500000)
        finally:os.environ.clear();os.environ.update(old)
        try:
            text=g.shell('streamcheck 120; echo STREAM_STATUS:$?',180)
            print(text,flush=True)
            assert 'failures=0' in text and 'STREAM_STATUS:0' in text,text
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('s=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            start=len(g.log)
            g.shell('NETSURF_TRACE_IMAGES=1 scrollcheck --trace --images http://tls.test/ &');g.shell('p=$!')
            text=g.shell('wait $p; echo SCROLL_STATUS:$?',300)
            print(text,flush=True)
            log=g.log[start:]
            assert 'SCROLL_STATUS:0' in text and b'NETSURF_SCROLL_CHECK finished' in log,text
            assert re.search(rb'NETSURF_SCROLL_CHECK \d+ previous_y=[1-9]\d*',log),log
            assert re.search(rb'NETSURF_SCROLL_CHECK .*loading=1',log),log
            assert b'NETSURF_IMAGE_REJECT' in log and b'too-wide.png' in log and b'unsupported.png' in log,log
            for line in log.splitlines():
                if b'NETSURF_IMAGE_REJECT' in line:
                    assert int(re.search(rb'prefix=(\d+)',line).group(1))<8192,line
            g.shell('kill $s; wait $s')
        finally:
            output.write_bytes(g.log)
            try:g.close()
            finally:peer.close()
    print(args.target+': parallel HTTP, header/prefix limits, live image scrolling and clean filesystem passed')


if __name__=='__main__':main()
