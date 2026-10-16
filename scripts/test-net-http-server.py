#!/usr/bin/env python3
"""Independent Ethernet active-open peer for the guest TCP/HTTP server.

Uses no host TCP stack. Checks SYN-ACK/data/FIN loss, duplicate final FIN after
compact TIME_WAIT, out-of-order request data, zero windows and malformed packets.
Build static HTTP_SERVER_DHCP=0, HTTP_SERVER_TIMEOUT=15000UL variants first.
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

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

wire = module('server_wire', 'scripts/test-net-smoke.py')
live = module('server_expected', 'scripts/test-net-http-server-live.py')
PAGE = live.expected_page()
RESPONSE = (f'HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=us-ascii\r\nContent-Length: {len(PAGE)}\r\nConnection: close\r\n\r\n').encode() + PAGE
REQUEST = b'GET / HTTP/1.1\r\nHost: guest.test\r\n\r\n'
MASK = 0xffffffff
CASES = ['success', 'loss', 'reorder', 'zero', 'malformed']


class Peer:
    def __init__(self, sock, case):
        self.sock, self.case, self.address = sock, case, None
        self.isn, self.server_isn = 0xfffffff0, None
        self.request_sent = self.request_acked = 0
        self.received = bytearray()
        self.syn_at = self.request_at = self.fin_at = 0
        self.window = 0 if case == 'zero' else 512
        self.handshake = self.fin = self.fin_sent = self.done = False
        self.syn_dropped = self.data_dropped = self.ack_dropped = self.fin_dropped = self.last_ack_dropped = False
        self.reordered = self.injected = False
        self.probes = self.late_acks = 0

    def send(self, frame):
        self.sock.sendto(frame, self.address)

    def frame(self, seq, flags=16, data=b'', corrupt=False):
        ack = 0 if self.server_isn is None else self.server_isn + 1 + len(self.received) + int(self.fin)
        options = b'\x02\x04\0\x71' if flags & 2 else b''
        p = bytearray(struct.pack('!HHIIBBHHH', 50000, 8080, seq & MASK, ack & MASK,
                                 (20 + len(options)) * 4, flags, self.window, 0, 0) + options + data)
        pseudo = wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 6, len(p))
        p[16:18] = struct.pack('!H', wire.checksum(pseudo + p) ^ int(corrupt))
        ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, len(p) + 20, 7, 0, 64, 6, 0, wire.HOST_IP, wire.GUEST_IP))
        ip[10:12] = struct.pack('!H', wire.checksum(ip))
        return wire.GUEST_MAC + wire.HOST_MAC + b'\x08\x00' + ip + p

    def ack(self):
        self.send(self.frame(self.isn + 1 + self.request_sent + int(self.fin_sent)))

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
        assert frame[12:15] == b'\x08\x00\x45' and wire.checksum(frame[14:34]) == 0 and frame[23] == 6
        assert frame[26:30] == wire.GUEST_IP and frame[30:34] == wire.HOST_IP
        p = frame[34:14 + total]
        sport, dport, seq, ack, offset, flags, window, _, _ = struct.unpack('!HHIIBBHHH', p[:20])
        assert sport == 8080 and dport == 50000 and not flags & 4, ('unexpected reset', flags)
        assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 6, len(p)) + p) == 0
        data = p[offset // 16 * 4:]
        if flags & 2:
            assert flags == 18 and ack == (self.isn + 1) & MASK and b'\x02\x04\x02\x00' in p[20:]
            if self.case == 'loss' and not self.syn_dropped:
                self.syn_dropped = True
                return
            if self.server_isn is not None:
                assert seq == self.server_isn, 'SYN-ACK changed ISN'
            self.server_isn = seq
            self.handshake = True
            self.ack()
            return
        assert self.handshake and flags & 16
        acked = (ack - self.isn - 1) & MASK
        assert acked <= len(REQUEST) + int(self.fin_sent), ('ACK beyond request', acked)
        self.request_acked = max(self.request_acked, min(acked, len(REQUEST)))
        if acked == len(REQUEST) + 1:
            if self.case == 'loss' and not self.last_ack_dropped:
                self.last_ack_dropped = True
            else:
                self.late_acks += int(self.last_ack_dropped)
                self.done = True
        if data:
            start = (seq - self.server_isn - 1) & MASK
            assert data == RESPONSE[start:start + len(data)], 'server stream differs'
            if self.case == 'zero' and not self.window:
                assert len(data) == 1 and start == 0
                self.probes += 1
                if self.probes >= 2:
                    self.window = 37
                self.ack()
                return
            if self.case == 'loss' and not self.data_dropped:
                self.data_dropped = True
                return
            if start <= len(self.received):
                self.received.extend(data[len(self.received) - start:])
            if self.case == 'loss' and not self.ack_dropped:
                self.ack_dropped = True
                return
            if self.case == 'zero' and len(self.received) > 100:
                self.window = 512
            self.ack()
        if flags & 1:
            assert (seq + len(data) - self.server_isn - 1) & MASK == len(RESPONSE)
            assert self.received == RESPONSE
            if self.case == 'loss' and not self.fin_dropped:
                self.fin_dropped = True
                return
            self.fin = True
            self.ack()

    def progress(self, ready):
        if not ready or self.done:
            return
        now = time.monotonic()
        if not self.handshake:
            if now - self.syn_at >= 0.5:
                if self.case == 'malformed' and not self.injected:
                    self.send(self.frame(self.isn, 2, corrupt=True))
                    self.injected = True
                self.send(self.frame(self.isn, 2))
                self.syn_at = now
        elif self.request_acked < len(REQUEST):
            if self.request_sent == self.request_acked or now - self.request_at >= 0.4:
                start = self.request_acked
                data = REQUEST[start:start + 8]
                if self.case == 'reorder' and not self.reordered:
                    self.send(self.frame(self.isn + 11, 24, REQUEST[10:20]))
                    self.reordered = True
                self.send(self.frame(self.isn + 1 + start, 24, data))
                self.request_sent = max(self.request_sent, start + len(data))
                self.request_at = now
        elif self.fin and now - self.fin_at >= 0.4:
            self.fin_sent = True
            self.send(self.frame(self.isn + 1 + len(REQUEST), 17))
            self.fin_at = now


def run(args, mode, case):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if mode == 'rvm' and args.jit else '') + '-' + case
    result = {'success': False, 'mode': mode, 'case': case}
    began = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='rserve-') as directory, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        path = str(Path(directory) / 'nic.sock')
        sock.bind(path)
        sock.settimeout(0.01)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(build / 'http-server')]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_PACED='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
        else:
            bank = int((build / 'bank-size').read_text())
            command = [str(ROOT / 'target/release/rvm'), str(build / 'http-server.bin'), '--net', path, '--clock', 'paced',
                       '--bank-size', str(bank), '--memory', str(bank * 8), '--headless', '--exit-code', '--timeout', str(args.timeout),
                       '--disk', str(build / (label + '.disk'))]
            if args.jit:
                command.append('--jit')
        log = build / (label + '.log')
        with log.open('w') as output, (build / (label + '-stderr.log')).open('w') as diagnostic:
            guest = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=diagnostic)
            peer = Peer(sock, case)
            try:
                while not peer.done or 'HTTP SERVER complete status=200' not in log.read_text():
                    if guest.poll() is not None or time.monotonic() - began > args.timeout:
                        raise RuntimeError('server wire test failed: ' + log.read_text())
                    try:
                        frame, source = sock.recvfrom(65536)
                    except socket.timeout:
                        peer.progress('HTTP SERVER ready' in log.read_text())
                        continue
                    if frame == b'RNET\x01':
                        peer.address = source
                        sock.sendto(b'RNET\x01' + wire.GUEST_MAC + struct.pack('!H', 1500), source)
                        peer.send(wire.arp(1, b'\xff' * 6, b'\0' * 6, wire.GUEST_IP))
                    else:
                        assert source == peer.address
                        peer.packet(frame)
                    peer.progress('HTTP SERVER ready' in log.read_text())
                assert peer.received == RESPONSE
                if case == 'loss':
                    assert all([peer.syn_dropped, peer.data_dropped, peer.ack_dropped, peer.fin_dropped, peer.last_ack_dropped]) and peer.late_acks
                if case == 'zero':
                    assert peer.probes >= 2
                if case == 'reorder':
                    assert peer.reordered
                result.update(success=True, response_octets=len(peer.received), probes=peer.probes, compact_wait_ack=bool(peer.late_acks))
                print(f'{label}: passed (independent TCP/HTTP wire peer)', flush=True)
            finally:
                guest.terminate()
                try:
                    guest.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
                result['elapsed_seconds'] = time.monotonic() - began
                (build / (label + '.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/http-server-wire')
    parser.add_argument('--timeout', type=int, default=60)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['native', 'rvm']:
        for case in args.case or CASES:
            run(args, mode, case)


if __name__ == '__main__':
    main()
