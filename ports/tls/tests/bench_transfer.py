#!/usr/bin/env python3
"""Compare HTTP/HTTPS body throughput over real guest Ethernet and local OpenSSL.

Uses disposable roots and validates every downloaded octet. Body time starts
when the server queues its response and ends when the guest command finishes;
it excludes connection setup but includes final parsing/file writes. The fixture
has one segment in flight, so these are controlled regression measurements, not
maximum-throughput claims. --delay-ms delays each response data frame once.
"""
import argparse,json,os,re,select,struct,tempfile,threading,time
from pathlib import Path
from fixtures import make
from guest import Guest,ROOT
from peer import Services


class DrivenGuest(Guest):
    def pump(self):
        # The peer has its own thread: terminal polling never paces Ethernet.
        if select.select([self.master],[],[],.001)[0]:
            try:self.log.extend(os.read(self.master,65536))
            except OSError:pass


class BulkPeer(Services):
    def __init__(self,path,certs,body,delay):
        self.delay=delay/1000;self.delayed=[]
        super().__init__(path,certs,body)
        self.sock.settimeout(.001)

    def http(self,s):
        if getattr(s,'answered',False) or b'\r\n\r\n' not in s.received:return
        s.answered=True;s.path='/binary';s.body_at=time.monotonic();s.chunk=512
        s.queue(b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(self.body)).encode()+
                b'\r\nConnection: close\r\n\r\n'+self.body)

    def tls(self,s):
        s.chunk=512
        super().tls(s)
        if getattr(s,'responded',False) and not hasattr(s,'body_at'):
            s.body_at=time.monotonic()

    def send(self,frame):
        if self.delay and len(frame)>=54 and frame[23]==6:
            offset=14+(frame[14]&15)*4
            source,target=struct.unpack('!HH',frame[offset:offset+4])
            payload=struct.unpack('!H',frame[16:18])[0]-(offset-14)-(frame[offset+12]>>4)*4
            stream=self.streams.get((source,target))
            if payload>0 and stream and hasattr(stream,'body_at'):
                self.delayed.append((time.monotonic()+self.delay,bytes(frame)));return
        super().send(frame)

    def pump(self):
        super().pump()
        while self.delayed and self.delayed[0][0]<=time.monotonic():
            _,frame=self.delayed.pop(0);super().send(frame)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['native','ripple'],default='native')
    p.add_argument('--executable',type=Path)
    p.add_argument('--size',type=int,default=65536)
    p.add_argument('--repeat',type=int,default=2)
    p.add_argument('--delay-ms',type=float,default=0)
    p.add_argument('--full-trust',action='store_true')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if not 1<=a.size<=262144 or a.repeat<1 or not 0<=a.delay_ms<=500:
        p.error('use size 1..262144, positive repeat and delay 0..500 ms')
    native=a.target=='native';build=ROOT/('fuse-x/build/apps' if native else 'build/fuzix')
    suffix='.fxe' if native else '.rxe'
    results=[]
    with tempfile.TemporaryDirectory(prefix='tls-transfer-',dir='/tmp') as temporary:
        d=Path(temporary);certs=make(d/'certs')
        body=(bytes(range(256))*((a.size+255)//256))[:a.size]
        expected=d/'expected';expected.write_bytes(body)
        trust=d/'trust.pem';trust.write_bytes((ROOT/'ports/tls/roots/cert.pem').read_bytes() if a.full_trust else b'')
        with trust.open('ab') as out:out.write((certs/'ca.pem').read_bytes())
        peer=BulkPeer(d/'nic',certs,body,a.delay_ms)
        stop=threading.Event();errors=[]
        def drive():
            try:
                while not stop.is_set():peer.pump()
            except BaseException as error:errors.append(repr(error));stop.set()
        worker=threading.Thread(target=drive,daemon=True);worker.start()
        g=None
        try:
            apps={'htget':a.executable or build/('htget'+suffix),'cmp':build/('cmp'+suffix)}
            files=[dict(name='/etc/ssl/cert.pem',path=str(trust)),dict(name='/expected',path=str(expected))]
            g=DrivenGuest(d,a.target,apps,files,peer,blocks=65536)
            for repeat in range(a.repeat):
                for scheme in ['http','https']:
                    previous={id(s) for s in list(peer.streams.values())};started=time.monotonic()
                    profile=g.shell('RIPPLE_TLS_PROFILE=1 htget '+scheme+'://tls.test/binary /result; echo TRANSFER_STATUS:$?',300)
                    ended=time.monotonic()
                    assert not errors,errors
                    assert 'TRANSFER_STATUS:0' in profile,profile
                    streams=[s for s in list(peer.streams.values()) if id(s) not in previous and hasattr(s,'body_at')]
                    assert len(streams)==1,len(streams)
                    duration=ended-streams[0].body_at
                    result=dict(target=a.target,scheme=scheme,repeat=repeat,size=a.size,
                                delay_ms=a.delay_ms,full_trust=a.full_trust,wall_seconds=ended-started,
                                body_seconds=duration,kib_s=a.size/1024/duration,profile=profile)
                    response=re.search(r'HTTP response detail: (\d+) body octets, (\d+) ms',profile)
                    if response:
                        assert int(response[1])==a.size,response[0]
                        result['response_ms']=int(response[2])
                        result['response_kib_s']=a.size*1000/1024/int(response[2]) if int(response[2]) else None
                    assert 'BODY_STATUS:0' in g.shell('cmp /expected /result; echo BODY_STATUS:$?')
                    results.append(result);print(json.dumps(result),flush=True)
        finally:
            try:
                if g:g.close()
            finally:
                stop.set();worker.join(5);peer.close()
        assert not errors,errors
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(results,indent=2)+'\n')


if __name__=='__main__':main()
