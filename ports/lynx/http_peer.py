"""HTTP/DNS fixtures on independent Ethernet, exercising the guest TCP stack."""
import struct
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fuzix/tools'))
from network_peer import Peer, wire

HOME = b'''<html><head><title>Guest network home</title></head><body>
<h1>Network browser ready</h1>
<p><a href="relative/next">Follow relative link</a></p>
<form action="/search" method="GET"><input name="q" size="24" maxlength="80">
<select name="mode"><option value="all">Everything</option><option value="text">Text</option></select>
<input type="submit" value="Search fixture"></form>
<p><a href="/redirect">Redirect test</a> <a href="/chunked">Chunked test</a></p>
</body></html>'''
NEXT = b'<html><title>Relative target</title><h1>Relative navigation passed</h1><a href="/">Home</a></html>'
CHUNKED = b'<html><title>Chunked response</title><h1>Fragmented chunked HTML passed</h1><p>Entities &amp; links work.</p></html>'


class Services(Peer):
    def __init__(self, path):
        super().__init__(path)
        self.tcp_services = {80: self.http}
        self.paths = []
        self.queries = []
        self.requests = []

    def packet(self, frame):
        if len(frame) >= 54 and frame[12:14] == b'\x08\x00' and frame[23] == 6:
            at = 14 + (frame[14] & 15) * 4
            source, target, sequence = struct.unpack('!HHI', frame[at:at + 8])
            if (target == 9999 or target == 443 and target not in self.tcp_services) and frame[at + 13] & 2:
                packet = bytearray(struct.pack('!HHIIBBHHH', target, source, 0,
                    (sequence + 1) & 0xffffffff, 80, 20, 0, 0, 0))
                check = wire.checksum(wire.HOST_IP + wire.GUEST_IP
                                     + struct.pack('!BBH', 0, 6, len(packet)) + packet)
                packet[16:18] = struct.pack('!H', check)
                self.send(self.ip(6, packet))
                return
        if len(frame) >= 42 and frame[12:14] == b'\x08\x00' and frame[23] == 17:
            at = 14 + (frame[14] & 15) * 4
            packet = frame[at:14 + struct.unpack('!H', frame[16:18])[0]]
            source, target, length, _ = struct.unpack('!HHHH', packet[:8])
            assert length == len(packet)
            assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, length) + packet) == 0
            if target != 53:
                return super().packet(frame)
            query, labels, at = packet[8:], [], 12
            while query[at]:
                count = query[at]
                labels.append(query[at + 1:at + 1 + count].decode())
                at += count + 1
            self.queries.append('.'.join(labels))
            assert query[at + 1:at + 5] == b'\0\x01\0\x01', query
            flags = 0x8183 if self.queries[-1] == 'missing.test' else 0x8180
            records = b'' if flags == 0x8183 else b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + wire.HOST_IP
            answer = query[:2] + struct.pack('!HHHHH', flags, 1, bool(records), 0, 0) + query[12:at + 5] + records
            response = bytearray(struct.pack('!HHHH', target, source, len(answer) + 8, 0) + answer)
            check = wire.checksum(wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 17, len(response)) + response)
            response[6:8] = struct.pack('!H', check or 65535)
            self.send(self.ip(17, response))
            return
        super().packet(frame)

    def http(self, stream):
        data = bytes(stream.received)
        if getattr(stream, 'answered', False) or b'\r\n\r\n' not in data:
            return
        stream.answered = True
        self.requests.append(data)
        method, path, version = data.split(b'\r\n', 1)[0].decode().split()
        assert method == 'GET' and version.startswith('HTTP/1.')
        assert b'Host: ' in data
        self.paths.append(path)
        stream.path = path
        headers = b''
        status = b'200 OK'
        if path == '/': body = HOME
        elif path == '/relative/next': body = NEXT
        elif path == '/redirect':
            status, headers, body = b'302 Found', b'Location: /chunked\r\n', b''
        elif path == '/loop':
            status, headers, body = b'302 Found', b'Location: /loop\r\n', b''
        elif path == '/chunked':
            stream.chunk = 7
            response = b'HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n'
            for at in range(0, len(CHUNKED), 13):
                chunk = CHUNKED[at:at + 13]
                response += f'{len(chunk):x};fixture=yes\r\n'.encode() + chunk + b'\r\n'
            stream.queue(response + b'0\r\nX-Fixture: done\r\n\r\n')
            return
        elif path.startswith('/search?'):
            params = parse_qs(urlsplit(path).query)
            assert params == {'q': ['retro & C16+'], 'mode': ['text']}, params
            body = b'<html><title>Search result</title><h1>GET form encoding passed</h1><p>retro &amp; C16+</p></html>'
        elif path == '/slow':
            stream.queue(b'HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>Waiting for cancellation</h1>')
            return
        elif path.startswith('/request?padding='):
            assert urlsplit(path).query == 'padding=' + 'x' * 1500, path
            body = b'<html><h1>Complete long request received</h1></html>'
        elif path == '/oversize':
            body = b'<html><pre>' + b'x' * 70000 + b'</pre></html>'
            stream.chunk = 512
            stream.queue(b'HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n' + body)
            return
        elif path == '/notfound':
            status, body = b'404 Not Found', b'<html><h1>Fixture page not found</h1></html>'
        elif path == '/long':
            body = b'<html><title>Scrolling fixture</title>' + b''.join(f'<p>Scroll row {n:03d}</p>'.encode() for n in range(150)) + b'</html>'
        else:
            raise AssertionError(path)
        stream.chunk = 29
        stream.queue(b'HTTP/1.0 ' + status + b'\r\nContent-Type: text/html\r\n' + headers
                     + b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)

    def pump(self):
        super().pump()
        for stream in self.streams.values():
            if (getattr(stream, 'answered', False) and stream.path != '/slow'
                    and not stream.pending and not stream.flight):
                stream.closing = True
                stream.progress()
