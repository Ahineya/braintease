#!/usr/bin/env python3
"""Independent Ethernet/TCP echo peer. No host TCP stack or Internet is used.

Build NET_DEMO=tcp-echo in build/network/tcp/{native,rvm} first.
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
spec = importlib.util.spec_from_file_location('tcp_wire', ROOT / 'scripts/test-net-smoke.py')
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
MASK = 0xffffffff
OCTETS = 4097
EXPECTED = bytes((i * 37 + 11) % 256 for i in range(OCTETS))
CASES = ['success', 'loss', 'reorder', 'zero', 'partial-ack', 'malformed', 'reset', 'early', 'refused']


class Peer:
    def __init__(self, sock, case):
        self.sock, self.case, self.address = sock, case, None
        self.port, self.isn, self.client_isn = 0, 0xfffffff0, None
        self.received = bytearray()
        self.echo_sent = self.echo_acked = 0
        self.pending = None
        self.fin_sent = self.fin_acked = self.client_fin = False
        self.syn_dropped = self.data_dropped = self.ack_dropped = False
        self.echo_dropped = self.fin_dropped = self.last_ack_dropped = False
        self.wait_duplicate = self.reordered = self.partial = self.injected = False
        self.zero_probes = self.duplicates = self.frames = 0
        self.window = 0 if case == 'zero' else 512
        self.guest_window = 512
        self.scheduled = []

    def send(self, frame):
        self.sock.sendto(frame, self.address)

    def frame(self, seq, ack, flags=16, payload=b'', options=b'', change=None):
        header = bytearray(struct.pack('!HHIIBBHHH', 7777, self.port, seq & MASK, ack & MASK,
                                       (20 + len(options)) * 4, flags, self.window, 0, 0) + options)
        if change == 'offset':
            header[12] = 16
        if change == 'port':
            header[3] ^= 1
        segment = header + payload
        pseudo = wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 6, len(segment))
        check = wire.checksum(pseudo + segment) ^ (1 if change == 'checksum' else 0)
        segment[16:18] = struct.pack('!H', check)
        ip = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(segment), 7,
                                   0x2000 if change == 'fragment' else 0, 64, 6, 0, wire.HOST_IP, wire.GUEST_IP))
        ip[10:12] = struct.pack('!H', wire.checksum(ip))
        return wire.GUEST_MAC + wire.HOST_MAC + b'\x08\x00' + ip + segment

    def ack(self):
        sequence = self.isn + 1 + self.echo_sent + (1 if self.fin_sent else 0)
        self.send(self.frame(sequence, self.client_isn + 1 + len(self.received) + (1 if self.client_fin else 0)))

    def syn(self, seq):
        if self.client_isn is None:
            self.client_isn = seq
        assert self.client_isn == seq, 'retransmitted SYN changed sequence'
        if self.case == 'refused':
            self.send(self.frame(0, seq + 1, 20))
            return
        if self.case == 'loss' and not self.syn_dropped:
            self.syn_dropped = True
            return
        if self.case == 'malformed' and not self.injected:
            for change in ['offset', 'port', 'checksum', 'fragment']:
                self.send(self.frame(self.isn, seq + 1, 18, options=b'\x02\x04\0\x89', change=change))
            self.send(self.frame(self.isn, seq + 1, 18, options=b'\x02\x01\0\0'))
            self.send(self.frame(self.isn, seq + 2, 20))
        self.send(self.frame(self.isn, seq + 1, 18, options=b'\x01\x01\x02\x04\0\x89\0\0'))

    def packet(self, frame):
        if frame == b'RNET\x02':
            self.send(frame)
            return
        if frame == b'RNET\x04':
            return
        self.frames += 1
        assert self.frames < 1500, 'packet flood'
        assert frame[6:12] == wire.GUEST_MAC
        if frame[12:14] == b'\x08\x06':
            assert frame[20:22] == b'\0\x01' and frame[38:42] in [wire.HOST_IP, wire.GUEST_IP]
            if frame[38:42] == wire.HOST_IP:
                self.send(wire.arp(2, wire.GUEST_MAC, wire.GUEST_MAC, wire.GUEST_IP))
            return
        assert frame[12:15] == b'\x08\x00\x45' and frame[23] == 6
        total = struct.unpack('!H', frame[16:18])[0]
        assert 40 <= total <= len(frame) - 14 and wire.checksum(frame[14:34]) == 0
        assert frame[26:30] == wire.GUEST_IP and frame[30:34] == wire.HOST_IP
        p = frame[34:14 + total]
        sport, dport, seq, ack, offset, flags, window, _, _ = struct.unpack('!HHIIBBHHH', p[:20])
        assert dport == 7777 and offset // 16 >= 5 and offset // 16 * 4 <= len(p)
        assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 6, len(p)) + p) == 0
        data = p[offset // 16 * 4:]
        self.guest_window = window
        if flags & 4:
            assert self.case == 'malformed', 'unexpected client RST'
            return
        if flags & 2:
            assert flags == 2 and 49152 <= sport <= 65535
            assert b'\x02\x04\x02\x00' in p[20:offset // 16 * 4], 'missing bounded MSS'
            self.port = sport
            self.syn(seq)
            return
        assert self.client_isn is not None and sport == self.port and flags & 16
        if self.case == 'reset':
            self.send(self.frame(self.isn + 1, self.client_isn + 1, 20))
            return
        # ACKs advance one ordered byte stream, independent of segment boundaries.
        acked = (ack - self.isn - 1) & MASK
        limit = self.echo_sent + (1 if self.fin_sent else 0)
        assert acked <= limit, ('ACK beyond sent data', acked, limit)
        if acked > self.echo_acked:
            self.echo_acked = min(acked, self.echo_sent)
        if self.fin_sent and acked == self.echo_sent + 1:
            self.fin_acked = True
        if self.pending and acked >= self.pending[0] + self.pending[1]:
            self.pending = None
        if self.case == 'malformed' and not self.injected:
            self.injected = True
            self.send(self.frame(self.isn + 2, self.client_isn + 1, 4))  # in-window, inexact RST
            self.send(self.frame(self.isn + 1025, self.client_isn + 1, 4))
            self.send(self.frame(self.isn + 1, self.client_isn + 100000, 16))
        if data:
            if self.case == 'zero' and not self.window:
                assert len(data) == 1 and seq == (self.client_isn + 1) & MASK
                self.zero_probes += 1
                if self.zero_probes >= 2:
                    self.window = 37
                self.ack()  # Opening the window does not acknowledge the probe byte.
                return
            if self.case == 'loss' and not self.data_dropped:
                self.data_dropped = True
                return
            start = (seq - self.client_isn - 1) & MASK
            assert start <= len(self.received), ('out-of-order guest send', start, len(self.received))
            assert data == EXPECTED[start:start + len(data)], 'guest stream corruption'
            if start < len(self.received):
                self.duplicates += 1
                self.wait_duplicate = False
            tail = len(self.received) - start
            if tail < len(data):
                accepted = data[tail:]
                if self.case == 'partial-ack' and not self.partial:
                    self.partial = True
                    accepted = accepted[:max(1, len(accepted) // 2)]
                self.received.extend(accepted)
            if self.case == 'loss' and not self.ack_dropped:
                self.ack_dropped = True
                self.wait_duplicate = True
                return
            if self.case == 'zero' and len(self.received) > 100:
                self.window = 512
            self.ack()
        if flags & 1:
            assert (seq - self.client_isn - 1) & MASK == len(self.received)
            assert self.fin_sent and self.fin_acked, 'demo must drain peer EOF before half-closing'
            self.client_fin = True
            if self.case == 'loss' and not self.last_ack_dropped:
                self.last_ack_dropped = True
            else:
                self.ack()

    def progress(self):
        now = time.monotonic()
        for deadline, frame in self.scheduled[:]:
            if now >= deadline:
                self.send(frame)
                self.scheduled.remove((deadline, frame))
        if self.client_isn is None or self.case in ['reset', 'refused'] or self.wait_duplicate:
            return
        if self.pending:
            if now - self.pending[3] >= 0.3:
                start, size, frame, _ = self.pending
                self.send(frame)
                self.pending = (start, size, frame, now)
            return
        limit = min(len(self.received), 137 if self.case == 'early' else OCTETS)
        if self.echo_sent < limit and self.guest_window:
            size = min(113, limit - self.echo_sent, self.guest_window)
            start = self.echo_sent
            frame = self.frame(self.isn + 1 + start, self.client_isn + 1 + len(self.received), 24, self.received[start:start + size])
            self.echo_sent += size
            self.pending = (start, size, frame, now)
            if self.case == 'reorder' and not self.reordered and size > 60:
                self.reordered = True
                self.send(self.frame(self.isn + 1 + start + 40, self.client_isn + 1 + len(self.received), 24,
                                     self.received[start + 40:start + 60]))
                self.scheduled += [(now + 0.05, frame), (now + 0.07, frame)]
            elif self.case == 'loss' and not self.echo_dropped:
                self.echo_dropped = True
            else:
                self.send(frame)
        elif self.echo_acked == limit and limit == (137 if self.case == 'early' else OCTETS) and not self.fin_sent:
            self.fin_sent = True
            frame = self.frame(self.isn + 1 + limit, self.client_isn + 1 + len(self.received), 17)
            self.pending = (limit, 1, frame, now)
            if self.case == 'loss' and not self.fin_dropped:
                self.fin_dropped = True
            else:
                self.send(frame)


def run_case(args, mode, case):
    build = args.build_root.resolve() / mode
    label = mode + ('-jit' if args.jit and mode == 'rvm' else '') + '-' + case
    log = build / (label + '.log')
    result = {'success': False, 'mode': mode, 'case': case}
    began = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='rtcp-') as directory, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock, log.open('w') as output:
        path = str(Path(directory) / 'nic.sock')
        sock.bind(path)
        sock.settimeout(0.01)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(build / 'tcp-echo')]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(args.timeout))
        else:
            bank = int((build / 'bank-size').read_text())
            command = [str(ROOT / 'target/release/rvm'), str(build / 'tcp-echo.bin'), '--net', path, '--clock', 'paced',
                       '--bank-size', str(bank), '--memory', str(bank * 8), '--headless', '--exit-code',
                       '--timeout', str(args.timeout), '--disk', str(build / 'fixture.disk')]
            if args.jit:
                command.append('--jit')
        guest = subprocess.Popen(command, env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        peer = Peer(sock, case)
        try:
            while guest.poll() is None:
                if time.monotonic() - began > args.timeout:
                    raise RuntimeError('TCP fixture timeout: ' + log.read_text())
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
            if case in ['reset', 'refused']:
                assert guest.returncode == 3 and 'TCP failed status=11' in content, content
            elif case == 'early':
                assert guest.returncode == 8 and 'early echo EOF' in content, content
            else:
                assert guest.returncode == 0, content
                assert peer.received == EXPECTED and peer.echo_acked == OCTETS and peer.client_fin and peer.fin_acked
                assert f'TCP OK written={OCTETS} acknowledged={OCTETS} received={OCTETS}' in content, content
                if case == 'loss':
                    assert all([peer.syn_dropped, peer.data_dropped, peer.ack_dropped, peer.echo_dropped,
                                peer.fin_dropped, peer.last_ack_dropped]) and peer.duplicates >= 1
                    assert int(content.split('retransmits=')[1].split()[0]) >= 4, content
                if case == 'zero':
                    assert peer.zero_probes >= 2
                if case == 'partial-ack':
                    assert peer.partial
                if case == 'reorder':
                    assert peer.reordered
            result.update(success=True, guest_octets=len(peer.received), echo_acked=peer.echo_acked,
                          frames=peer.frames, duplicate_sends=peer.duplicates, zero_probes=peer.zero_probes)
            print(f'{label}: passed ({len(peer.received)} guest stream octets)', flush=True)
        finally:
            if guest.poll() is None:
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
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/tcp')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--timeout', type=int, default=45)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    for mode in args.mode or ['rvm', 'native']:
        for case in args.case or CASES:
            run_case(args, mode, case)


if __name__ == '__main__':
    main()
