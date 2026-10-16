#!/usr/bin/env python3
"""Independent Ethernet peer proving guest DNS -> TCP -> HTTP, without vmnet.

Build a static http-get guest with HTTP_HOST="web.ripple.test", HTTP_PORT=8080
in build/network/http-dns/{native,rvm}. This fixture never resolves a host name
or opens a host TCP socket. Ordinary host interoperability is a separate test.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('http_wire', ROOT / 'scripts/test-net-smoke.py')
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
MASK = 0xffffffff
BODY = b'<html>Guest-resolved HTTP</html>\n' * 310
RESPONSE = f'HTTP/1.0 200 OK\r\nContent-Length: {len(BODY)}\r\n\r\n'.encode() + BODY
REQUEST = b'GET / HTTP/1.0\r\nHost: web.ripple.test:8080\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n'


class Peer:
    def __init__(self, sock):
        self.sock, self.address = sock, None
        self.port, self.client_isn, self.isn = 0, None, 0xfffffff0
        self.request = bytearray()
        self.sent = self.acked = 0
        self.pending = None
        self.fin = self.client_fin = self.dns = False
        self.window = 512

    def send(self, frame):
        self.sock.sendto(frame, self.address)

    def ip(self, protocol, source, payload):
        ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 7, 0, 64, protocol, 0, source, wire.GUEST_IP))
        ip[10:12] = struct.pack('!H', wire.checksum(ip))
        return wire.GUEST_MAC + wire.HOST_MAC + b'\x08\x00' + ip + payload

    def frame(self, seq, flags=16, payload=b'', options=b''):
        ack = self.client_isn + 1 + len(self.request) + int(self.client_fin)
        segment = bytearray(struct.pack('!HHIIBBHHH', 8080, self.port, seq & MASK, ack & MASK,
                                       (20 + len(options)) * 4, flags, 512, 0, 0) + options + payload)
        pseudo = wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 6, len(segment))
        segment[16:18] = struct.pack('!H', wire.checksum(pseudo + segment))
        return self.ip(6, wire.HOST_IP, segment)

    def packet(self, frame):
        if frame == b'RNET\x02':
            self.send(frame)
            return
        if frame == b'RNET\x04':
            return
        if frame[12:14] == b'\x08\x06':
            if frame[38:42] == wire.HOST_IP:
                self.send(wire.arp(2, wire.GUEST_MAC, wire.GUEST_MAC, wire.GUEST_IP))
            return
        total = struct.unpack('!H', frame[16:18])[0]
        assert frame[12:15] == b'\x08\x00\x45' and wire.checksum(frame[14:34]) == 0
        assert frame[26:30] == wire.GUEST_IP and 20 <= total <= len(frame) - 14
        p = frame[34:14 + total]
        if frame[23] == 17:
            sport, dport, length, check = struct.unpack('!HHHH', p[:8])
            assert dport == 53 and frame[30:34] == b'\x01\x01\x01\x01' and length == len(p)
            assert check and wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, len(p)) + p) == 0
            query = p[8:]
            assert query[2:12] == b'\x01\x00\x00\x01' + b'\0' * 6
            assert query[12:] == b'\x03web\x06ripple\x04test\0\0\x01\0\x01'
            answer = query[:2] + b'\x81\x80\0\x01\0\x01\0\0\0\0' + query[12:]
            answer += b'\xc0\x0c\0\x01\0\x01\0\0\0\x3c\0\x04' + wire.HOST_IP
            udp = bytearray(struct.pack('!HHHH', 53, sport, len(answer) + 8, 0) + answer)
            source = b'\x01\x01\x01\x01'
            check = wire.checksum(source + wire.GUEST_IP + struct.pack('!BBH', 0, 17, len(udp)) + udp)
            udp[6:8] = struct.pack('!H', check or 65535)
            self.send(self.ip(17, source, udp))
            self.dns = True
            return
        assert frame[23] == 6 and frame[30:34] == wire.HOST_IP and self.dns
        sport, dport, seq, ack, offset, flags, window, _, _ = struct.unpack('!HHIIBBHHH', p[:20])
        assert dport == 8080 and wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 6, len(p)) + p) == 0
        assert 5 <= offset // 16 <= len(p) // 4
        data = p[offset // 16 * 4:]
        self.window = window
        if flags & 2:
            if self.client_isn is None:
                self.client_isn, self.port = seq, sport
            assert seq == self.client_isn and sport == self.port
            self.send(self.frame(self.isn, 18, options=b'\x02\x04\0\x89'))
            return
        assert not flags & 4, 'unexpected reset'
        assert sport == self.port and flags & 16
        acked = (ack - self.isn - 1) & MASK
        assert acked <= self.sent + int(self.fin), 'ACK beyond sent response'
        self.acked = max(self.acked, acked)
        if self.pending and acked >= self.pending[0]:
            self.pending = None
        if data:
            start = (seq - self.client_isn - 1) & MASK
            assert start <= len(self.request) and data == REQUEST[start:start + len(data)]
            self.request.extend(data[len(self.request) - start:])
            self.send(self.frame(self.isn + 1 + self.sent))
        if flags & 1:
            assert seq == (self.client_isn + 1 + len(self.request)) & MASK
            assert self.acked >= len(RESPONSE), 'guest closed before response was delivered'
            self.client_fin = True
            self.send(self.frame(self.isn + 1 + self.sent + int(self.fin)))

    def progress(self):
        now = time.monotonic()
        if self.pending:
            limit, frame, sent_at = self.pending
            if now - sent_at >= 0.3:
                self.send(frame)
                self.pending = limit, frame, now
            return
        if self.request != REQUEST:
            return
        if self.sent < len(RESPONSE) and self.window:
            # Vary the slice size, including single octets in header delimiters.
            size = min(1 + self.sent % 137, len(RESPONSE) - self.sent, self.window)
            frame = self.frame(self.isn + 1 + self.sent, 24, RESPONSE[self.sent:self.sent + size])
            self.sent += size
            self.pending = self.sent, frame, now
            self.send(frame)
        elif self.acked == len(RESPONSE) and not self.fin:
            self.fin = True
            frame = self.frame(self.isn + 1 + self.sent, 17)
            self.pending = self.sent + 1, frame, now
            self.send(frame)


def run(args, mode):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if mode == 'rvm' and args.jit else '')
    result = {'success': False, 'mode': mode}
    began = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='rhttp-') as directory, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        path = str(Path(directory) / 'nic.sock')
        sock.bind(path)
        sock.settimeout(0.01)
        peer = Peer(sock)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(build / 'http-get')]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
        else:
            bank = int((build / 'bank-size').read_text())
            command = [str(ROOT / 'target/release/rvm'), str(build / 'http-get.bin'), '--net', path, '--clock', 'paced',
                       '--headless', '--exit-code', '--timeout', str(args.timeout), '--bank-size', str(bank),
                       '--memory', str(bank * 8), '--disk', str(build / 'fixture.disk')]
            if args.jit:
                command.append('--jit')
        log = build / (label + '-dns-http.log')
        with log.open('w') as output, (build / (label + '-stderr.log')).open('w') as diagnostic:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=diagnostic)
            try:
                while guest.poll() is None:
                    if time.monotonic() - began > args.timeout:
                        raise RuntimeError('DNS/HTTP fixture timeout: ' + log.read_text())
                    try:
                        frame, source = sock.recvfrom(65536)
                    except socket.timeout:
                        peer.progress()
                        continue
                    if frame == b'RNET\x01':
                        peer.address = source
                        sock.sendto(b'RNET\x01' + wire.GUEST_MAC + struct.pack('!H', 1500), source)
                    else:
                        assert source == peer.address
                        peer.packet(frame)
                    peer.progress()
                content = log.read_text()
                assert guest.returncode == 0, content
                assert peer.dns and peer.request == REQUEST and peer.client_fin
                assert 'HTTP BODY\n' + BODY.decode() + '\nHTTP END\n' in content, 'streamed body differs'
                assert f'HTTP OK status=200 received={len(BODY)} checksum={sum(BODY)}' in content, content
                result.update(success=True, received=len(BODY), dns=True, response_acked=peer.acked)
                print(f'{label}: guest DNS -> TCP -> HTTP, {len(BODY)} body octets verified', flush=True)
            finally:
                if guest.poll() is None:
                    guest.terminate()
                    try:
                        guest.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        guest.kill()
                        guest.wait()
                result['elapsed_seconds'] = time.monotonic() - began
                (build / (label + '-dns-http.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/http-dns')
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--jit', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['native', 'rvm']:
        run(args, mode)


if __name__ == '__main__':
    main()
