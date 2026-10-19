#!/usr/bin/env python3
"""Riplix HTTP, DNS and UDP tools against independent Ethernet fixtures (JIT)."""
import argparse
from launch import require_mmu
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import time
from network_peer import Peer, wire

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)
BINARY = bytes(range(256)) * 16 + b'!'
NTP_TIME = 1900000000

def name(value):
    if value == '.': return b'\0'
    return b''.join(bytes([len(s)]) + s.encode() for s in value.split('.')) + b'\0'

def question(packet):
    at, parts = 12, []
    while packet[at]:
        n = packet[at];at += 1;parts.append(packet[at:at+n].decode().lower());at += n
    at += 1
    return '.'.join(parts) or '.', struct.unpack('!H', packet[at:at+2])[0], at+4

def rr(owner, kind, data, ttl=70000):
    return owner + struct.pack('!HHIH', kind, 1, ttl, len(data)) + data

def dns_reply(query, tcp=False):
    host, kind, end = question(query)
    q = query[12:end]
    flags, records = 0x8180, []
    if host == 'missing.test': flags |= 3
    elif host == 'large.test' and not tcp: flags |= 0x200
    elif host == 'bad.test': records = [rr(b'\xc0\xff', 1, wire.HOST_IP)]
    elif host == 'loop.test': records = [rr(b'\xc0\x0c', 5, name(host))]
    elif host == 'poison.test': records = [rr(name('unrelated.test'), 1, wire.HOST_IP)]
    elif host == 'chain.test': records = [rr(b'\xc0\x0c', 5, name('alias.test'))]
    elif host == 'alias.test':
        # Deliberately put the final A record before its CNAME.
        records = [rr(name('fuzix.test'), 1, wire.HOST_IP), rr(b'\xc0\x0c', 5, name('fuzix.test'))]
    elif kind == 12: records = [rr(b'\xc0\x0c', 12, name('fuzix.test'))]
    elif kind == 28: records = [rr(b'\xc0\x0c', 28, bytes.fromhex('20010db8000000000000000000000001'))]
    elif kind == 15: records = [rr(b'\xc0\x0c', 15, b'\0\x0a' + name('mail.fuzix.test'))]
    elif kind == 2: records = [rr(b'\xc0\x0c', 2, name('ns.fuzix.test'))]
    elif kind == 6:
        records = [rr(b'\xc0\x0c', 6, name('ns.fuzix.test') + name('admin.fuzix.test') + struct.pack('!IIIII', 4000000000, 70000, 80000, 90000, 100000))]
    elif host == 'high.test':
        records = [rr(b'\xc0\x0c', 16, b'\xfa' + b'x'*250)]
        offset = 12 + len(q) + len(records[0])
        records.append(rr(name('deep.high.test'), 1, wire.HOST_IP))
        assert offset > 255
        records.append(rr(struct.pack('!H', 0xc000 | offset), 1, wire.HOST_IP))
    elif kind == 16: records = [rr(b'\xc0\x0c', 16, b'\x0bhello world\x03a\0b')]
    else: records = [rr(b'\xc0\x0c', 1, wire.HOST_IP)]
    return query[:2] + struct.pack('!HHHHH', flags, 1, len(records), 0, 0) + q + b''.join(records)

class Services(Peer):
    def __init__(self, path):
        super().__init__(path)
        self.tcp_services = {80: self.http, 53: self.dns_tcp, 9999: self.reject}
        self.queries = []
        self.paths = []
        self.fallbacks = 0

    def udp(self, port, target, data):
        packet = bytearray(struct.pack('!HHHH', port, target, len(data)+8, 0) + data)
        check = wire.checksum(wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 17, len(packet)) + packet)
        packet[6:8] = struct.pack('!H', check or 65535)
        self.send(self.ip(17, packet))

    def packet(self, frame):
        if len(frame)>=54 and frame[12:14]==b'\x08\x00' and frame[23]==6:
            at=14+(frame[14]&15)*4
            source,target,seq=struct.unpack('!HHI',frame[at:at+8])
            if target==9999 and frame[at+13]&2:
                packet=bytearray(struct.pack('!HHIIBBHHH',target,source,0,(seq+1)&0xffffffff,80,20,0,0,0))
                check=wire.checksum(wire.HOST_IP+wire.GUEST_IP+struct.pack('!BBH',0,6,len(packet))+packet)
                packet[16:18]=struct.pack('!H',check);self.send(self.ip(6,packet));return
        if len(frame) >= 42 and frame[12:14] == b'\x08\x00' and frame[23] == 17:
            header = (frame[14] & 15) * 4
            payload = frame[14+header:14+struct.unpack('!H', frame[16:18])[0]]
            source, port, size, _ = struct.unpack('!HHHH', payload[:8])
            assert size == len(payload)
            assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, size) + payload) == 0
            data = payload[8:]
            if port == 53:
                self.queries.append(question(data)[:2])
                answer = dns_reply(data)
                # Stay within the guest's two-datagram queue while injecting
                # an unrelated ID or question before the legitimate answer.
                if question(data)[0] == 'fuzix.test':
                    bad=bytearray(answer)
                    if len(self.queries)&1: bad[0]^=1
                    else: bad[13]^=1
                    self.udp(port, source, bad)
                self.udp(port, source, answer)
            elif port == 7: self.udp(port, source, data)
            elif port == 123:
                assert len(data) == 48 and data[0] == 0x23
                reply = bytearray(48);reply[0]=0x24;reply[1]=1
                reply[24:32]=data[40:48]
                reply[40:44]=struct.pack('!I', (NTP_TIME+2208988800)&0xffffffff)
                self.udp(port, source, reply[:24] + b'badnonce' + reply[32:])
                self.udp(port, source, reply)
            else: raise AssertionError(('unexpected UDP service', port))
            return
        super().packet(frame)

    def dns_tcp(self, stream):
        data = bytes(stream.received)
        if getattr(stream, 'answered', False) or len(data)<2: return
        size = struct.unpack('!H',data[:2])[0]
        if len(data)<size+2: return
        stream.answered=True;self.fallbacks+=1
        reply=dns_reply(data[2:2+size], tcp=True)
        stream.chunk=7;stream.queue(struct.pack('!H',len(reply))+reply);stream.closing=True

    def reject(self, stream):
        stream.send(stream.next, 20);stream.reset=True

    def http(self, stream):
        data = bytes(stream.received)
        if getattr(stream, 'answered', False) or b'\r\n\r\n' not in data: return
        stream.answered=True
        path=data.split(b' ')[1].decode();self.paths.append(path)
        assert b'Host: ' in data and b'Accept-Encoding: identity\r\n' in data
        if path in ('/binary','/relative/../binary'):
            response=b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(BINARY)).encode()+b'\r\n\r\n'+BINARY
        elif path=='/chunked':
            response=b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n'
            for at in range(0,len(BINARY),137):
                chunk=BINARY[at:at+137];response+=f'{len(chunk):x};fixture=yes\r\n'.encode()+chunk+b'\r\n'
            response+=b'0\r\nX-Fixture: complete\r\n\r\n'
        elif path=='/info':
            response=b'HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </binary>\r\n\r\nHTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\nINFO_BODY!\n'
        elif path in ('/redirect','/relative/start','/loop'):
            target={'/redirect':'/chunked','/relative/start':'../binary','/loop':'/loop'}[path]
            response=f'HTTP/1.1 302 Found\r\nLocation: {target}\r\nContent-Length: 0\r\n\r\n'.encode()
        elif path=='/notfound': response=b'HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\nnope'
        elif path=='/badlength': response=b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc'
        elif path=='/badheader': response=b'HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n'
        elif path=='/longheader': response=b'HTTP/1.1 200 OK\r\nX: '+b'x'*1100+b'\r\n\r\n'
        elif path=='/chunkbad': response=b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort'
        elif path=='/gzip': response=b'HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\ncompressed'
        elif path=='/bad204': response=b'HTTP/1.1 204 No Content\r\nContent-Length: 100\r\n\r\n'
        elif path=='/hang':
            stream.queue(b'HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial')
            return
        elif path=='/close': response=b'HTTP/1.0 200 OK\r\n\r\nCLOSE_BODY\n'
        else: raise AssertionError(path)
        stream.chunk=29;stream.queue(response);stream.closing=True

with tempfile.TemporaryDirectory(prefix='fuzix-nettools-') as directory:
    tmp=Path(directory)
    names=['init','sh','echo','cat','cksum','cmp','ls','sleep','kill','date','htget','dig','host','nslookup','echoping','ntpdate','ping']
    for file in [*(n+'.rxe' for n in names),'FUZIX.SYS','bios.bin']:(tmp/file).symlink_to(build/file)
    (tmp/'apps.json').write_text(json.dumps(names))
    image=tmp/'tools.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(tmp),str(image)],check=True)
    commands=['echo nameserver 192.168.64.1 > /etc/resolv.conf',
              'echo 192.168.64.1 local.test >> /etc/hosts',
              'host local.test','host fuzix.test','host alias.test','host chain.test','host large.test',
              'nslookup fuzix.test','host 192.168.64.1',
              'dig -x 192.168.64.1',
              'dig @192.168.64.1 fuzix.test A','dig fuzix.test AAAA +short',
              'dig fuzix.test MX','dig fuzix.test NS','dig fuzix.test TXT','dig fuzix.test SOA',
              'dig high.test TXT','dig missing.test A',
              'host missing.test','echo NX_STATUS:$?', 'host poison.test','echo POISON_STATUS:$?',
              'host loop.test','echo LOOP_DNS_STATUS:$?', 'host bad.test','echo BAD_DNS_STATUS:$?',
              'ping -c 1 fuzix.test','echo NAME_PING:$?',
              'echoping fuzix.test 7 RIPPLE_ECHO','echo ECHO_STATUS:$?',
              "echoping fuzix.test 7 ''",'echo EMPTY_ECHO:$?',
              'ntpdate -d fuzix.test','echo NTP_STATUS:$?',
              'ntpdate -s fuzix.test','echo NTP_SET:$?',
              'ntpdate -o 65536 fuzix.test','echo NTP_BAD_OFFSET:$?',
              'host fuzix.test','echo DNS_AFTER_CLOCK:$?']
    for path in ['binary','chunked','redirect','relative/start','info','close']:
        file=path.replace('/','-')
        commands += [f'htget http://fuzix.test/{path} /{file}',f'echo HTTP_{file.upper()}:$?',f'cksum /{file}']
    commands += ['echo KEEP_ORIGINAL > /keep']
    commands += ['htget http://fuzix.test:9999/binary /keep','echo REFUSED_STATUS:$?']
    for path in ['notfound','badlength','badheader','longheader','chunkbad','gzip','bad204','loop']:
        commands += [f'htget http://fuzix.test/{path} /keep',f'echo FAIL_{path.upper()}:$?','cat /keep']
    commands += ['htget http://192.168.64.1/hang /keep &','client=$!','sleep 2',
                 'echo HTTP_PARTIAL_FILE','ls -a /','kill -2 $client','wait $client',
                 'echo HTTP_INTERRUPT:$?','cat /keep','ls -a /','shutdown']
    with tempfile.TemporaryDirectory(prefix='fnt-',dir='/tmp') as sockets:
        peer=Services(Path(sockets)/'nic')
        log=Path('/tmp/fuzix-nettools.log')
        memory='16384000'
        with log.open('w') as output:
            guest=subprocess.Popen([str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
                '--memory',memory,'--net',str(Path(sockets)/'nic'),'--headless','--jit','--input','\n'.join(commands)+'\n',
                '--timeout','420'],stdout=output,stderr=subprocess.STDOUT)
            try:
                deadline=time.monotonic()+430
                while guest.poll() is None:
                    assert time.monotonic()<deadline,log.read_text()[-6000:]
                    peer.pump()
                assert guest.returncode==0,log.read_text()[-8000:]
            finally:
                if guest.poll() is None:
                    guest.terminate();guest.wait(timeout=10)
                peer.close()
        text=log.read_text().replace('\r','')
        assert 'disk synced; power off.' in text and 'panic:' not in text and 'Segmentation' not in text,text[-12000:]
        for token in ['local.test has address 192.168.64.1','fuzix.test has address 192.168.64.1',
            'large.test has address 192.168.64.1','mail.fuzix.test.','hello world','deep.high.test.',
            '4000000000','NXDOMAIN','2001:db8:0:0:0:0:0:1','NX_STATUS:1','POISON_STATUS:1',
            'LOOP_DNS_STATUS:1','BAD_DNS_STATUS:1','NAME_PING:0','ECHO_STATUS:0','EMPTY_ECHO:0','NTP_STATUS:0','NTP_SET:0',
            'DNS_AFTER_CLOCK:0','HTTP_INTERRUPT:130','REFUSED_STATUS:1','NTP_BAD_OFFSET:1']:
            assert token in text,(token,text[-18000:])
        for path in ['BINARY','CHUNKED','REDIRECT','RELATIVE-START','INFO','CLOSE']:
            assert f'HTTP_{path}:0\n' in text,(path,text[-16000:])
        check=subprocess.check_output(['cksum'],input=BINARY).decode().split()
        for file in ['binary','chunked','redirect','relative-start']:
            assert f'{check[0]} {len(BINARY)} /{file}' in text,(file,text[-16000:])
        for path in ['NOTFOUND','BADLENGTH','BADHEADER','LONGHEADER','CHUNKBAD','GZIP','BAD204','LOOP']:
            assert f'FAIL_{path}:1\n' in text,(path,text[-10000:])
        assert text.count('KEEP_ORIGINAL\n')>=8,text[-8000:]
        partial=text.split('HTTP_PARTIAL_FILE\n')[-1].split('riplix# kill -2')[0]
        assert '.htget' in partial,partial
        listing=text.split('riplix# ls -a /')[-1]
        assert '.htget' not in listing,listing
        assert peer.fallbacks>=1 and ('chain.test',1) in peer.queries
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    reboot=subprocess.run([str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory',memory,
        '--net','off','--headless','--jit','--input','cksum /binary\ncat /keep\ncat /etc/resolv.conf\nshutdown\n',
        '--timeout','90'],capture_output=True,text=True,timeout=100)
    assert reboot.returncode==0 and f'{check[0]} {len(BINARY)} /binary' in reboot.stdout and 'KEEP_ORIGINAL' in reboot.stdout,reboot.stdout
    assert 'nameserver 192.168.64.1' in reboot.stdout,reboot.stdout
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
print('Riplix HTTP/DNS tools: binary/chunked downloads, redirects, failure safety, aliases, TCP DNS, NTP, interrupts and persistence passed')
