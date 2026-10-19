"""Blind public TCP relay on the private Ethernet fixture. Never decrypts TLS."""
import ipaddress
from pathlib import Path
import socket
import sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/lynx'))
from http_peer import Services

def server_name(data):
    """Read SNI from complete TLS handshake records without a TLS endpoint."""
    handshake=bytearray();at=0
    while at+5<=len(data):
        kind=data[at];n=int.from_bytes(data[at+3:at+5],'big')
        if at+5+n>len(data):return None
        if kind!=22:raise ValueError('expected TLS ClientHello')
        handshake.extend(data[at+5:at+5+n]);at+=5+n
        if len(handshake)>=4:
            size=int.from_bytes(handshake[1:4],'big')
            if len(handshake)>=size+4:break
    else:return None
    b=handshake
    if b[0]!=1:raise ValueError('expected ClientHello')
    p=4+2+32;p+=1+b[p]
    p+=2+int.from_bytes(b[p:p+2],'big');p+=1+b[p]
    end=p+2+int.from_bytes(b[p:p+2],'big');p+=2
    while p+4<=end:
        kind=int.from_bytes(b[p:p+2],'big');n=int.from_bytes(b[p+2:p+4],'big');p+=4
        if kind==0:
            q=p+2
            while q+3<=p+n:
                typ=b[q];length=int.from_bytes(b[q+1:q+3],'big');q+=3
                if typ==0:return bytes(b[q:q+length]).decode('ascii')
                q+=length
        p+=n
    raise ValueError('public TLS test requires SNI')

class Relay(Services):
    def __init__(self,path):
        super().__init__(path)
        self.tcp_services={80:self.forward,443:self.forward}
        self.sockets=[];self.hosts=[];self.failures=[]
    def forward(self,stream):
        try:
            if not hasattr(stream,'remote_socket'):
                data=bytes(stream.received)
                if len(data)>65536:raise ValueError('oversized initial request')
                if stream.local==443:host=server_name(data)
                else:
                    if b'\r\n\r\n' not in data:return
                    fields=[l.split(b':',1)[1].strip().decode('ascii') for l in data.split(b'\r\n') if l.lower().startswith(b'host:')]
                    if len(fields)!=1:raise ValueError('expected one Host header')
                    host=fields[0].split(':')[0]
                if not host:return
                addresses=socket.getaddrinfo(host,stream.local,socket.AF_INET,socket.SOCK_STREAM)
                # A public-web test must never turn untrusted redirects into
                # requests to private host services or metadata endpoints.
                address=next(a[4] for a in addresses if ipaddress.ip_address(a[4][0]).is_global)
                stream.remote_socket=socket.create_connection(address,timeout=10)
                stream.remote_socket.setblocking(False);self.sockets.append(stream.remote_socket)
                stream.forwarded=0;stream.remote_eof=False
                self.hosts.append((stream.local,host));print('Public TCP relay:',stream.local,host,flush=True)
            self.progress_relay(stream)
        except (OSError,ValueError,StopIteration,IndexError) as error:
            self.failures.append(str(error));stream.remote_eof=stream.closing=True
    def progress_relay(self,stream):
        if stream.reset or stream.remote_eof:return
        try:
            pending=stream.received[stream.forwarded:]
            if pending:
                try:stream.forwarded+=stream.remote_socket.send(pending)
                except BlockingIOError:pass
            if len(stream.pending)<65536:
                try:
                    data=stream.remote_socket.recv(16384)
                    if data:stream.queue(data)
                    else:stream.remote_eof=stream.closing=True
                except BlockingIOError:pass
        except OSError as error:
            self.failures.append(str(error));stream.remote_eof=stream.closing=True
    def pump(self):
        super().pump()
        for stream in self.streams.values():
            if hasattr(stream,'remote_socket'):self.progress_relay(stream)
    def close(self):
        for connection in self.sockets:connection.close()
        super().close()
