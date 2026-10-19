"""Ordinary OpenSSL TLS server on the independent Ethernet fixture."""
from pathlib import Path
import ssl
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'lynx'))
from http_peer import Services as HTTPServices

BINARY=bytes(range(256))*16+b'Ripple TLS\0\xff\n'


class Services(HTTPServices):
    def __init__(self,path,fixtures,body=BINARY,content_type=b'application/octet-stream'):
        self.body,self.content_type=body,content_type
        super().__init__(path)
        self.contexts={}
        self.sni=[];self.versions=[];self.failures=[]
        for port,version,certificate in [(443,ssl.TLSVersion.TLSv1_3,'server'),(444,ssl.TLSVersion.TLSv1_2,'server'),
                (445,ssl.TLSVersion.TLSv1_3,'expired'),(446,ssl.TLSVersion.TLSv1_3,'p384'),
                (447,ssl.TLSVersion.TLSv1_3,'p521'),(448,ssl.TLSVersion.TLSv1_3,'rsa'),
                (449,ssl.TLSVersion.TLSv1_2,'rsa'),(450,ssl.TLSVersion.TLSv1_3,'cross-chain'),
                (451,ssl.TLSVersion.TLSv1_3,'wrong-san'),(452,ssl.TLSVersion.TLSv1_3,'no-san')]:
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version=context.maximum_version=version
            context.load_cert_chain(str(fixtures/(certificate+'.pem')),str(fixtures/('server.key' if certificate in ('expired','cross-chain','wrong-san','no-san') else certificate+'.key')))
            if certificate in ('p384','p521'):
                context.set_ecdh_curve('secp384r1' if certificate=='p384' else 'secp521r1')
            context.set_servername_callback(lambda ssl,name,ctx:self.sni.append(name))
            self.contexts[port]=context
            self.tcp_services[port]=self.tls

    def tls(self,stream):
        if not hasattr(stream,'tls'):
            stream.incoming=ssl.MemoryBIO();stream.outgoing=ssl.MemoryBIO()
            stream.tls=self.contexts[stream.local].wrap_bio(stream.incoming,stream.outgoing,server_side=True)
            stream.tls_at=0;stream.tls_ready=False;stream.request=bytearray();stream.responded=False
            stream.tls_started=time.monotonic()
        fresh=bytes(stream.received[stream.tls_at:]);stream.tls_at=len(stream.received)
        if fresh:stream.incoming.write(fresh)
        try:
            if not stream.tls_ready:
                stream.tls.do_handshake()
                stream.tls_ready=True
                self.versions.append(stream.tls.version())
                print(f'Peer handshake {stream.local}: {stream.tls.version()} {stream.tls.cipher()[0]} after {time.monotonic()-stream.tls_started:.2f}s',flush=True)
            while not stream.responded:
                stream.request.extend(stream.tls.read(16384))
                if b'\r\n\r\n' not in stream.request:continue
                stream.responded=True
                path=bytes(stream.request).split(b' ')[1].decode()
                stream.tls_path=path
                print(f'Peer request {stream.local} {path} after {time.monotonic()-stream.tls_started:.2f}s',flush=True)
                self.paths.append(path);self.requests.append(bytes(stream.request))
                if path=='/redirect':
                    response=b'HTTP/1.1 302 Found\r\nLocation: /binary\r\nContent-Length: 0\r\n\r\n'
                elif path=='/downgrade':
                    response=b'HTTP/1.1 302 Found\r\nLocation: http://tls.test/binary\r\nContent-Length: 0\r\n\r\n'
                elif path=='/truncated':
                    response=b'HTTP/1.1 200 OK\r\nContent-Length: 9999\r\n\r\nshort'
                elif path in ('/unclean','/hang'):
                    response=b'HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nshort'
                elif path=='/chunked':
                    response=b'HTTP/1.1 200 OK\r\nContent-Type: '+self.content_type+b'\r\nTransfer-Encoding: chunked\r\n\r\n'
                    for at in range(0,len(self.body),137):
                        part=self.body[at:at+137]
                        response+=f'{len(part):x}\r\n'.encode()+part+b'\r\n'
                    response+=b'0\r\n\r\n'
                else:
                    assert path in ('/binary','/tampered'),path
                    response=b'HTTP/1.1 200 OK\r\nContent-Type: '+self.content_type+b'\r\nContent-Length: '+str(len(self.body)).encode()+b'\r\n\r\n'+self.body
                stream.tls.write(response)
                if path in ('/truncated','/unclean','/tampered'):stream.closing=True
                elif path=='/hang':pass
                else:
                    try:stream.tls.unwrap()
                    except ssl.SSLWantReadError:pass
                    stream.closing=True
        except ssl.SSLWantReadError:pass
        except ssl.SSLError as error:
            self.failures.append(str(error));stream.closing=True
        finally:
            while stream.outgoing.pending:
                data=stream.outgoing.read()
                if getattr(stream,'tls_path',None)=='/tampered':
                    data=data[:-1]+bytes([data[-1]^1])
                stream.queue(data)
