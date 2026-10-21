#!/usr/bin/env python3
"""Verify process-local A caching, normalization, alias TTL and invalidation."""
import collections
from pathlib import Path
import struct
import sys
import tempfile
import subprocess
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/lynx'))
from http_peer import Services, wire

class DNS(Services):
    def packet(self,frame):
        if len(frame)>=42 and frame[12:14]==b'\x08\x00' and frame[23]==17:
            at=14+(frame[14]&15)*4
            source,target,length,_=struct.unpack('!HHHH',frame[at:at+8])
            if target==53:
                q=frame[at+8:at+length];end=12;labels=[]
                while q[end]:
                    n=q[end];labels.append(q[end+1:end+1+n].decode());end+=1+n
                name='.'.join(labels);self.queries.append(name);end+=5
                records=b'';count=1
                if name=='alias.test':
                    value=b'\x06target\x04test\0'
                    records=b'\xc0\x0c'+struct.pack('!HHIH',5,1,1,len(value))+value
                    records+=value+struct.pack('!HHIH',1,1,120,4)+wire.HOST_IP;count=2
                else:
                    assert name in ('cache.test','zero.test'),name
                    records=b'\xc0\x0c'+struct.pack('!HHIH',1,1,0 if name=='zero.test' else 120,4)+wire.HOST_IP
                answer=q[:2]+struct.pack('!HHHHH',0x8180,1,count,0,0)+q[12:end]+records
                udp=bytearray(struct.pack('!HHHH',53,source,len(answer)+8,0)+answer)
                check=wire.checksum(wire.HOST_IP+wire.GUEST_IP+struct.pack('!BBH',0,17,len(udp))+udp)
                udp[6:8]=struct.pack('!H',check or 65535);self.send(self.ip(17,udp));return
        super().packet(frame)

def main():
    build=ROOT/'build/dns-cache';build.mkdir(exist_ok=True)
    subprocess.run(['python3',str(ROOT/'fuse-x/cc.py'),str(Path(__file__).with_name('dnscachecheck.c')),'-o',str(build/'check.fxe')],check=True)
    with tempfile.TemporaryDirectory(prefix='dns-cache-',dir='/tmp') as folder:
        peer=DNS(Path(folder)/'nic');guest=None
        try:
            guest=Guest(folder,'native',{'dnscache':build/'check.fxe'},peer=peer,blocks=65536)
            text=guest.shell('dnscache; echo DNS_STATUS:$?',60);print(text,flush=True)
            assert 'failures=0' in text and 'DNS_STATUS:0' in text,text
            counts=collections.Counter(peer.queries)
            assert counts=={'cache.test':2,'zero.test':2,'alias.test':2},counts
            print('DNS_CACHE_QUERIES',dict(counts))
        finally:
            if guest:(build/'guest.log').write_bytes(guest.log);guest.close()
            peer.close()
if __name__=='__main__':main()
