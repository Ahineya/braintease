#!/usr/bin/env python3
"""Opt-in public HTTPS interop: a blind TCP relay, with all TLS in the guest.

DNS in this private Ethernet fixture maps example.com to the test peer. The
relay neither decrypts TLS nor substitutes certificates or HTTP responses.
This tests public trust/interop, not the normal vmnet/NAT routing path.
"""
import argparse
from pathlib import Path
import socket
import sys
import tempfile
import time
from guest import Guest, ROOT
sys.path.insert(0, str(ROOT / 'ports/lynx'))
from http_peer import Services as HTTPServices


class Relay(HTTPServices):
    def __init__(self, path, hostname):
        super().__init__(path)
        self.hostname = hostname
        self.tcp_services[443] = self.forward
        self.sockets = []

    def forward(self, stream):
        if not hasattr(stream, 'remote_socket'):
            stream.remote_socket = socket.create_connection((self.hostname, 443), timeout=10)
            stream.remote_socket.setblocking(False)
            stream.forwarded = 0
            stream.remote_eof = False
            stream.relay_started = time.monotonic()
            stream.relay_received = 0
            self.sockets.append(stream.remote_socket)
        self.progress_relay(stream)

    def progress_relay(self, stream):
        if stream.reset or stream.remote_eof:
            return
        pending = stream.received[stream.forwarded:]
        if pending:
            try:
                stream.forwarded += stream.remote_socket.send(pending)
            except BlockingIOError:
                pass
        if len(stream.pending) < 65536:
            try:
                data = stream.remote_socket.recv(16384)
                if data:
                    stream.relay_received += len(data)
                    stream.queue(data)
                else:
                    stream.remote_eof = stream.closing = True
                    print(f'Remote EOF after {time.monotonic()-stream.relay_started:.2f}s; sent {stream.forwarded}, received {stream.relay_received} TLS octets',flush=True)
            except BlockingIOError:
                pass

    def pump(self):
        super().pump()
        for stream in self.streams.values():
            if hasattr(stream, 'remote_socket'):
                self.progress_relay(stream)

    def close(self):
        for connection in self.sockets:
            connection.close()
        super().close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--host', default='example.com')
    parser.add_argument('--path', default='/')
    parser.add_argument('--expect', default='Example Domain')
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--software', action='store_true')
    args = parser.parse_args()
    executable = args.executable or ROOT / 'build/tls' / ('htget.fxe' if args.target == 'native' else 'htget.rxe')
    with tempfile.TemporaryDirectory(prefix='ripple-public-https-') as temporary:
        directory = Path(temporary)
        peer = Relay(directory / 'nic', args.host)
        guest = Guest(directory, args.target, {'htget': executable},
            [dict(name='/etc/ssl/cert.pem', path=str(ROOT / 'ports/tls/roots/cert.pem'))], peer)
        try:
            settings=('RIPPLE_TLS_PROFILE=1 ' if args.profile else '')+('RIPPLE_TLS_SOFTWARE=1 ' if args.software else '')
            text = guest.shell(f'{settings}htget https://{args.host}{args.path} /public.html; echo PUBLIC_STATUS:$?', 300)
            print(text, flush=True)
            assert 'PUBLIC_STATUS:0' in text, text
            text = guest.shell('cat /public.html', 60)
            print(text, flush=True)
            assert args.expect in text, text
        finally:
            guest.close()
            peer.close()
    print('Public HTTPS with guest TLS and pinned Mozilla roots passed')


if __name__ == '__main__':
    main()
