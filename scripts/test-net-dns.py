#!/usr/bin/env python3
"""Independent Ethernet/UDP/DNS oracle. No host DNS resolver or Internet used.

Build DNS demos first (scripts/build-net-ping.sh with NET_DEMO=dns-ping).
RBF execution is deliberately opt-in.
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
spec = importlib.util.spec_from_file_location('packet_oracle', ROOT / 'scripts/test-net-smoke.py')
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
DNS = bytes([1, 1, 1, 1])
TARGET = bytes([203, 0, 113, 9])
CASES = ['success', 'retry', 'timeout', 'nxdomain', 'truncated', 'servfail', 'no-address']


def name(value):
    return b''.join(bytes([len(label)]) + label.encode('ascii') for label in value.split('.')) + b'\0'


def ip_packet(payload, protocol=17, source=DNS, fragment=0):
    header = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 7, fragment, 64, protocol, 0, source, wire.GUEST_IP)
    header = header[:10] + struct.pack('!H', wire.checksum(header)) + header[12:]
    return wire.GUEST_MAC + wire.HOST_MAC + b'\x08\x00' + header + payload


def udp_packet(data, port, source=DNS, source_port=53, corrupt=False, size=None, fragment=0):
    udp = struct.pack('!HHHH', source_port, port, len(data) + 8 if size is None else size, 0) + data
    pseudo = source + wire.GUEST_IP + struct.pack('!BBH', 0, 17, len(udp))
    checksum = wire.checksum(pseudo + udp) or 65535
    if corrupt:
        checksum ^= 1
    udp = udp[:6] + struct.pack('!H', checksum) + udp[8:]
    return ip_packet(udp, source=source, fragment=fragment)


def rr(owner, kind, data, ttl=90):
    return owner + struct.pack('!HHIH', kind, 1, ttl, len(data)) + data


def response(query, answers=b'', count=0, flags=0x8180, identifier=None, question=None):
    return struct.pack('!HHHHHH', struct.unpack('!H', query[:2])[0] if identifier is None else identifier,
                       flags, 1, count, 0, 0) + (query[12:] if question is None else question) + answers


def run_case(mode, case, timeout, jit):
    build = ROOT / 'build/network/dns' / mode
    label = mode + ('-jit' if jit else '') + '-' + case
    log = build / (label + '.log')
    result = {'success': False, 'case': case, 'mode': mode, 'queries': [], 'frames': 0}
    start = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='rdns-') as tmp, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as peer, log.open('w') as output:
        path = str(Path(tmp) / 'nic.sock')
        peer.bind(path)
        peer.settimeout(0.1)
        env = os.environ.copy()
        if mode == 'native':
            command = [str(build / 'dns-ping')]
            env.update(RCC_NATIVE_NET=path, RCC_NATIVE_PACED='1', RCC_NATIVE_HEADLESS='1', RCC_NATIVE_TIMEOUT=str(timeout))
        else:
            command = [str(ROOT / 'target/release' / mode), str(build / ('dns-ping.bf' if mode == 'rbf' else 'dns-ping.bin')),
                       '--net', path, '--headless', '--exit-code', '--timeout', str(timeout)]
            if mode == 'rvm':
                bank = int((build / 'bank-size').read_text())
                command += ['--bank-size', str(bank), '--memory', str(bank * 8), '--clock', 'paced', '--disk', str(build / 'fixture.disk')]
            if jit:
                command += ['--jit']
        guest = subprocess.Popen(command, env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        client = None
        replies = []
        try:
            while guest.poll() is None:
                if time.monotonic() - start > timeout:
                    raise RuntimeError('fixture timed out: ' + log.read_text())
                try:
                    frame, source = peer.recvfrom(65536)
                except socket.timeout:
                    continue
                if frame == b'RNET\x01':
                    client = source
                    peer.sendto(b'RNET\x01' + wire.GUEST_MAC + struct.pack('!H', 1500), client)
                    continue
                assert client == source
                if frame == b'RNET\x02':
                    peer.sendto(frame, client)
                    continue
                if frame == b'RNET\x04':
                    continue
                result['frames'] += 1
                assert result['frames'] < 64, 'packet flood'
                assert frame[6:12] == wire.GUEST_MAC
                if frame[12:14] == b'\x08\x06':
                    assert frame[20:22] == b'\0\x01'
                    assert frame[38:42] in [wire.GUEST_IP, wire.HOST_IP]
                    if frame[38:42] == wire.HOST_IP:
                        peer.sendto(wire.arp(2, wire.GUEST_MAC, wire.GUEST_MAC, wire.GUEST_IP), client)
                    continue
                assert frame[:6] == wire.HOST_MAC and frame[12:14] == b'\x08\x00'
                total = struct.unpack('!H', frame[16:18])[0]
                assert wire.checksum(frame[14:34]) == 0 and total <= len(frame) - 14
                assert frame[26:30] == wire.GUEST_IP
                payload = frame[34:14 + total]
                if frame[23] == 1:
                    assert frame[30:34] == TARGET and wire.checksum(payload) == 0
                    kind, code, _, identifier, seq = struct.unpack('!BBHHH', payload[:8])
                    assert kind == 8 and code == 0 and identifier == 0x5246 and seq == len(replies) + 1
                    assert payload[8:] == bytes(255 if i == 1 else i for i in range(17))
                    replies.append(seq)
                    peer.sendto(ip_packet(wire.echo(0, identifier, seq, payload[8:]), 1, TARGET), client)
                    continue
                assert frame[23] == 17 and frame[30:34] == DNS
                port, destination, size, check = struct.unpack('!HHHH', payload[:8])
                assert 49152 <= port <= 65535 and destination == 53 and size == len(payload) and check != 0
                pseudo = wire.GUEST_IP + DNS + struct.pack('!BBH', 0, 17, size)
                assert wire.checksum(pseudo + payload) == 0
                query = payload[8:]
                identifier, flags, qd, an, ns, ar = struct.unpack('!HHHHHH', query[:12])
                assert (flags, qd, an, ns, ar) == (0x100, 1, 0, 0, 0)
                assert query[12:] == name('syma-lang.org') + b'\0\x01\0\x01'
                assert identifier not in result['queries'], 'retry reused transaction ID'
                result['queries'].append(identifier)
                if case == 'timeout' or (case == 'retry' and len(result['queries']) == 1):
                    continue
                pointer = b'\xc0\x0c'
                if case in ['success', 'retry']:
                    if case == 'success':
                        # Correct checksums but wrong tuple, ID or question must be ignored.
                        valid = response(query, rr(pointer, 1, TARGET), 1)
                        bad = [udp_packet(valid, port, source=bytes([1, 0, 0, 1])),
                               udp_packet(valid, port, source_port=54), udp_packet(valid, port - 1),
                               udp_packet(response(query, identifier=identifier ^ 1), port),
                               udp_packet(response(query, question=name('other.example') + b'\0\x01\0\x01'), port),
                               udp_packet(valid, port, corrupt=True), udp_packet(valid, port, size=7),
                               udp_packet(valid, port, fragment=0x2000)]
                        offset = 12 + len(query[12:])
                        # Self/forward/header pointers, cyclic names, truncated RDATA and excessive counts.
                        malformed = [response(query, rr(struct.pack('!H', 0xc000 | offset), 1, TARGET), 1),
                                     response(query, rr(b'\xc0\xff', 1, TARGET), 1),
                                     response(query, rr(b'\xc0\x00', 1, TARGET), 1),
                                     response(query, b'\x01a' + struct.pack('!H', 0xc000 | offset) + b'\0' * 10, 1),
                                     response(query, rr(pointer, 1, TARGET)[:-1], 1),
                                     response(query, count=65535),
                                     response(query, rr(pointer, 5, b'\xc0\x0c'), 1)]
                        for packet in bad + [udp_packet(data, port) for data in malformed]:
                            peer.sendto(packet, client)
                    # Out-of-order A before CNAME; unrelated A must never win.
                    records = rr(name('unrelated.example'), 1, bytes([203, 0, 113, 99]))
                    records += rr(name('alias.example'), 1, TARGET, 120)
                    records += rr(pointer, 5, name('alias.example'), 45)
                    peer.sendto(udp_packet(response(query, records, 3), port), client)
                elif case == 'nxdomain':
                    peer.sendto(udp_packet(response(query, flags=0x8183), port), client)
                elif case == 'truncated':
                    peer.sendto(udp_packet(response(query, flags=0x8380), port), client)
                elif case == 'servfail':
                    peer.sendto(udp_packet(response(query, flags=0x8182), port), client)
                elif case == 'no-address':
                    peer.sendto(udp_packet(response(query, rr(name('unrelated.example'), 1, TARGET), 1), port), client)
            content = log.read_text()
            if case in ['success', 'retry']:
                assert guest.returncode == 0, content
                assert 'DNS answer name=syma-lang.org address=203.0.113.9 ttl=45' in content, content
                assert replies == [1, 2, 3] and 'PING OK received=3 responded=0' in content, content
                assert len(result['queries']) == (2 if case == 'retry' else 1)
            else:
                status = {'nxdomain': 2, 'truncated': 5, 'servfail': 4, 'no-address': 3, 'timeout': 6}[case]
                assert guest.returncode == 6 and f'DNS failed status={status} ' in content, content
                assert len(result['queries']) == (3 if case == 'timeout' else 1)
                assert not replies
            result['success'] = True
            print(f'{label}: passed ({len(result["queries"])} guest DNS queries)')
        finally:
            if guest.poll() is None:
                guest.terminate()
                try:
                    guest.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
            result['elapsed_seconds'] = time.monotonic() - start
            (build / (label + '.json')).write_text(json.dumps(result, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['rvm', 'native', 'rbf'], action='append')
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--timeout', type=int, default=30)
    parser.add_argument('--jit', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    for mode in args.mode or ['rvm', 'native']:
        for case in args.case or CASES:
            run_case(mode, case, args.timeout, args.jit and mode != 'native')


if __name__ == '__main__':
    main()
