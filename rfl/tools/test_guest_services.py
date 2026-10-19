#!/usr/bin/env python3
"""Real guest socket/reactor/crypto acceptance, using disposable disks and Ethernet."""
import argparse
import os
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
sys.path.insert(0, str(ROOT / 'fuse-x'))
from guest import Guest
from test_network import Services
from network_peer import wire


def dns_name(name):
    return b''.join(bytes([len(label)]) + label.encode() for label in name.split('.')) + b'\0'


def dns_question(query):
    at, labels = 12, []
    while query[at]:
        length = query[at]
        labels.append(query[at + 1:at + 1 + length].decode())
        at += length + 1
    return '.'.join(labels), at + 5


def dns_reply(query, tcp=False):
    name, end = dns_question(query)
    flags, records = 0x8180, []
    def record(owner, kind, value):
        return owner + struct.pack('!HHIH', kind, 1, 60, len(value)) + value
    if name == 'missing.fixture':
        flags |= 3
    elif name in ('tcp.fixture', 'oversize.fixture', 'tcp-bad.fixture') and not tcp:
        flags |= 0x200
    elif name == 'alias.fixture':
        records.append(record(b'\xc0\x0c', 5, dns_name('middle.fixture')))
    elif name == 'middle.fixture':
        records.append(record(dns_name('final.fixture'), 1, wire.HOST_IP))
        records.append(record(b'\xc0\x0c', 5, dns_name('final.fixture')))
    elif name == 'loop.fixture':
        records.append(record(b'\xc0\x0c', 5, b'\xc0\x0c'))
    elif name == 'poison.fixture':
        records.append(record(dns_name('unrelated.fixture'), 1, wire.HOST_IP))
    elif name != 'empty.fixture':
        records.append(record(b'\xc0\x0c', 1, wire.HOST_IP))
    reply = query[:2] + struct.pack('!HHHHH', flags, 1, len(records), 0, 0) + query[12:end] + b''.join(records)
    if tcp and name == 'tcp-bad.fixture':
        reply = bytes([reply[0] ^ 1]) + reply[1:]
    return reply


class ServicePeer(Services):
    def __init__(self, path):
        super().__init__(path)
        self.tcp_services[53] = self.dns_tcp
        self.pending_dns = []
        self.query_names = []
        self.gate_open = False
        self.gated = []

    def dns_tcp(self, stream):
        if len(stream.received) < 2 or getattr(stream, 'dns_answered', False):
            return
        size = struct.unpack('!H', stream.received[:2])[0]
        if len(stream.received) < size + 2:
            return
        stream.dns_answered = True
        query = bytes(stream.received[2:2 + size])
        name, _ = dns_question(query)
        if name == 'oversize.fixture':
            stream.queue(struct.pack('!H', 4097))
        else:
            reply = dns_reply(query, tcp=True)
            stream.chunk = 1  # Split the two-octet TCP length and body across packets.
            stream.queue(struct.pack('!H', len(reply)) + reply)

    def pump(self):
        super().pump()
        if self.gate_open:
            for packet in self.gated:
                self.send(packet)
            self.gated = []
        due = [x for x in self.pending_dns if x[0] <= time.monotonic()]
        self.pending_dns = [x for x in self.pending_dns if x[0] > time.monotonic()]
        for _, packet in due:
            self.send(packet)

    def packet(self, frame):
        if len(frame) >= 42 and frame[12:14] == b'\x08\x00' and frame[23] == 17:
            at = 14 + (frame[14] & 15) * 4
            source, target, length, _ = struct.unpack('!HHHH', frame[at:at + 8])
            if target == 53:
                query = frame[at + 8:at + length]
                assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, length) + frame[at:at + length]) == 0
                name, _ = dns_question(query)
                self.dns_queries += 1
                self.query_names.append(name)
                if name == 'silent.fixture' or frame[30:34] != wire.HOST_IP:
                    return
                if name == 'retry.fixture' and self.query_names.count(name) == 1:
                    return
                def packet(data):
                    payload = bytearray(struct.pack('!HHHH', 53, source, len(data) + 8, 0) + data)
                    check = wire.checksum(wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 17, len(payload)) + payload)
                    payload[6:8] = struct.pack('!H', check or 65535)
                    return self.ip(17, payload)
                reply = dns_reply(query)
                if name == 'gated.fixture':
                    self.gated.append(packet(reply))
                elif name == 'delayed.fixture':
                    # An unrelated transaction and a malformed matching reply must not
                    # complete the future or reset its absolute deadline.
                    self.send(packet(bytes([reply[0] ^ 1]) + reply[1:]))
                    self.send(packet(reply[:-1]))
                    self.pending_dns.append((time.monotonic() + .08, packet(reply)))
                else:
                    self.send(packet(reply))
                return
        if len(frame) >= 54 and frame[12:14] == b'\x08\x00' and frame[23] == 6:
            at = 14 + (frame[14] & 15) * 4
            source, target, seq = struct.unpack('!HHI', frame[at:at + 8])
            if target == 7002 and frame[at + 13] & 2:
                packet = bytearray(struct.pack('!HHIIBBHHH', 7002, source, 0,
                    (seq + 1) & 0xffffffff, 80, 20, 0, 0, 0))
                pseudo = wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 6, len(packet))
                packet[16:18] = struct.pack('!H', wire.checksum(pseudo + packet))
                self.send(self.ip(6, packet))
                return
        super().packet(frame)


class ResponsiveGuest(Guest):
    def pump(self):
        if b'RFL_ASYNC_TIMER_TRIGGER' in self.log:
            self.peer.gate_open = True
        super().pump()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple', 'both'), default='native')
    parser.add_argument('--rfl', type=Path, default=ROOT / 'target/release/rfl')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    targets = ('native', 'ripple') if args.target == 'both' else (args.target,)
    build = ROOT / 'build/guest-services'
    build.mkdir(parents=True, exist_ok=True)
    for target in targets:
        artifact = build / ('probe.fxe' if target == 'native' else 'probe.rxe')
        async_artifact = build / ('async.fxe' if target == 'native' else 'async.rxe')
        if not args.skip_build:
            subprocess.run([sys.executable, str(ROOT / 'rfl/tools/build_os.py'),
                str(ROOT / 'rfl/tests/fixtures/guest_services.rfl'), '--os',
                'fuse-x' if target == 'native' else 'fuzix', '--rfl', str(args.rfl.resolve()),
                '-o', str(artifact)], check=True)
            subprocess.run([sys.executable, str(ROOT / 'rfl/tools/build_os.py'),
                str(ROOT / 'rfl/tests/fixtures/guest_async.rfl'), '--os',
                'fuse-x' if target == 'native' else 'fuzix', '--rfl', str(args.rfl.resolve()),
                '-o', str(async_artifact)], check=True)
        # Short paths fit macOS Unix-domain socket address bounds.
        with tempfile.TemporaryDirectory(prefix='rfl-services-', dir='/tmp') as directory:
            peer = ServicePeer(Path(directory) / 'nic')
            guest = None
            try:
                guest = ResponsiveGuest(directory, target, {'probe': artifact, 'async': async_artifact},
                    files=[dict(name='/etc/hosts', text='10.20.30.40 fromhosts.fixture\n')], peer=peer)
                for argument, marker in [('', 'LOCAL'), ('crypto', 'CRYPTO'), ('network', 'NETWORK')]:
                    output = guest.shell(f'probe {argument}; echo SERVICE_STATUS:$?', 180)
                    print(output, flush=True)
                    assert f'RFL_GUEST_{marker}_OK' in output and 'SERVICE_STATUS:0' in output, output
                os.write(guest.master, b'probe terminal; echo SERVICE_STATUS:$?\r')
                guest.read(b'RFL_TERMINAL_WAITING')
                os.write(guest.master, b'ready\r')
                output = guest.read(guest.prompt)
                print(output, flush=True)
                assert 'RFL_GUEST_TERMINAL_OK' in output and 'SERVICE_STATUS:0' in output, output
                output = guest.shell('async; echo ASYNC_STATUS:$?', 180)
                print(output, flush=True)
                assert 'RFL_GUEST_ASYNC_OK' in output and 'ASYNC_STATUS:0' in output, output
                assert peer.query_names.count('retry.fixture') >= 2
                assert 'fromhosts.fixture' not in peer.query_names and '127.0.0.1' not in peer.query_names
                output = guest.shell("echo 'nameserver 192.168.64.2' > /etc/resolv.conf; echo 'nameserver 192.168.64.1' >> /etc/resolv.conf; async failover; echo FAILOVER_STATUS:$?", 180)
                print(output, flush=True)
                assert 'RFL_GUEST_FAILOVER_OK' in output and 'FAILOVER_STATUS:0' in output, output
                assert 'tcp.fixture' in peer.query_names and 'middle.fixture' in peer.query_names
                assert peer.dns_queries > 0
                streams = [s for s in peer.streams.values() if s.local == 7001]
                assert len(streams) >= 2
                assert all(bytes(s.received) == bytes(range(256)) * 32 for s in streams)
            finally:
                if guest is not None:
                    guest.close()
                peer.close()
        print(f'Guest services passed on {target}', flush=True)


if __name__ == '__main__':
    main()
