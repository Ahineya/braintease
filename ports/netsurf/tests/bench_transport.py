#!/usr/bin/env python3
"""Measure real guest HTTP/TLS under bounded bursts and artificial wire RTT.

The fixture is deliberately not a production TCP implementation: one burst
of up to the advertised receive window is outstanding, cumulatively ACKed.
It preserves real Ethernet/IP/TCP, socket syscalls, HTTP and guest TLS. The
ordinary regression fixture's single-segment sender remains unchanged.
"""
import argparse
import errno
import hashlib
import heapq
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_fetch import TimedGuest, Services
import network_peer
from fixtures import make

BaseStream = network_peer.Stream


class BurstStream(BaseStream):
    def progress(self):
        if not self.established or self.closed or self.reset or self.flight:
            return super().progress()
        if self.pending and self.window:
            count = min(self.owner.burst, self.window, len(self.pending))
            data = bytes(self.pending[:count])
            del self.pending[:count]
            # A single flight record permits a cumulative ACK for the burst.
            # Each actual Ethernet packet still respects the guest's MSS.
            start = self.next
            for at in range(0, count, 1460):
                self.send(start + at, 24, data[at:at + 1460])
            self.flight = (start, 24, data, time.monotonic())
            self.next = (start + count) & network_peer.MASK
        else:
            super().progress()

    def send(self, seq, flags=16, data=b''):
        # The base retry path may supply an entire burst; never emit a jumbo
        # frame during a retransmission either.
        for at in range(0, max(1, len(data)), 1460):
            super().send(seq + at, flags, data[at:at + 1460])


class TransportPeer(Services):
    def __init__(self, path, cert, size, burst):
        self.burst = burst
        self.delay = 0
        self.delayed = []
        self.serial = 0
        self.server_data = self.guest_data = self.frames_sent = 0
        self.windows = []
        super().__init__(path, cert)
        self.sock.setblocking(False)
        self.assets['/binary'] = (b'application/octet-stream', bytes(range(256)) * (size // 256))

    @staticmethod
    def tcp(frame):
        if len(frame) < 54 or frame[12:14] != b'\x08\x00' or frame[23] != 6:
            return None
        at = 14 + (frame[14] & 15) * 4
        end = 14 + int.from_bytes(frame[16:18], 'big')
        n = end - at - (frame[at + 12] >> 4) * 4
        return at, n

    def enqueue(self, direction, frame):
        self.serial += 1
        heapq.heappush(self.delayed, (time.monotonic() + self.delay / 2000, self.serial, direction, frame))

    def send(self, frame):
        info = self.tcp(frame)
        if info:
            self.server_data += info[1]
            self.frames_sent += 1
            self.enqueue('out', frame)
        else:
            super().send(frame)

    def pump(self):
        # Drain a finite batch rather than imposing one fixture wakeup on
        # every packet. Keep timers and host observation responsive.
        for _ in range(64):
            try:
                frame, source = self.sock.recvfrom(65536)
            except BlockingIOError:
                break
            if self.address is None:
                assert frame == b'RNET\x01'
                self.address = source
            assert source == self.address
            info = self.tcp(frame)
            if info:
                at, n = info
                self.guest_data += n
                self.windows.append(int.from_bytes(frame[at + 14:at + 16], 'big'))
                self.enqueue('in', frame)
            else:
                self.packet(frame)
        now = time.monotonic()
        while self.delayed and self.delayed[0][0] <= now:
            _, _, direction, frame = heapq.heappop(self.delayed)
            if direction == 'in':
                self.packet(frame)
            else:
                try:
                    super().send(frame)
                except OSError as error:
                    if error.errno not in (errno.ENOBUFS, errno.EAGAIN):
                        raise
                    # macOS Unix datagrams have a small packet queue. Honor
                    # transport backpressure rather than losing test packets.
                    heapq.heappush(self.delayed, (now + .001, self.serial + 1, direction, frame))
                    self.serial += 1
                    break
        for stream in list(self.streams.values()):
            stream.progress()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build/apps')
    p.add_argument('--rtt', type=int, nargs='+', default=[0, 20, 80])
    p.add_argument('--size', type=int, default=262144)
    p.add_argument('--burst', type=int, default=4096)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    assert args.size > 0 and args.size % 256 == 0
    assert 1 <= args.burst <= 32768 and all(0 <= r <= 500 for r in args.rtt)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    network_peer.Stream = BurstStream
    with tempfile.TemporaryDirectory(prefix='netsurf-transport-', dir='/tmp') as folder:
        work = Path(folder)
        cert = make(work / 'cert')
        trust = work / 'roots.pem'
        trust.write_bytes((ROOT / 'ports/tls/roots/cert.pem').read_bytes() + (cert / 'ca.pem').read_bytes())
        peer = TransportPeer(work / 'nic', cert, args.size, args.burst)
        original = args.build.resolve() / 'htget.fxe'
        app = work / 'htget.fxe'
        app.write_bytes(original.read_bytes())
        metadata = {}
        for name, path in dict(htget=app, kernel=Path(os.environ.get('FUSE_X_TEST_KERNEL', ROOT / 'fuse-x/build/fuse-x')),
                               libc=ROOT/'fuse-x/build/lib/libc.fxl.1', tls=ROOT/'fuse-x/build/lib/libtls.fxl.1').items():
            metadata[name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        guest = None
        try:
            guest = TimedGuest(work, 'native', {'htget': app},
                [dict(name='/etc/ssl/cert.pem', path=str(trust))], peer, blocks=131072)
            for repeat in range(args.repeat):
                for rtt in args.rtt:
                    peer.delay = rtt
                    for scheme in ('http', 'https'):
                        start = time.monotonic()
                        before = peer.server_data
                        frames = peer.frames_sent
                        at = len(peer.windows)
                        text = guest.shell('RIPPLE_TLS_PROFILE=1 htget ' + scheme + '://tls.test/binary /dev/null; echo TRANSFER_STATUS:$?', 180)
                        elapsed = time.monotonic() - start
                        assert 'TRANSFER_STATUS:0' in text, text
                        body, ms = map(int, re.findall(r'HTTP response detail: (\d+) body octets, (\d+) ms', text)[-1])
                        assert body == args.size and ms > 0, text
                        row = dict(repeat=repeat, scheme=scheme, rtt_ms=rtt, burst=args.burst, size=args.size,
                            wall_seconds=elapsed, response_ms=ms, kib_s=args.size / 1024 / (ms / 1000),
                            wire_octets=peer.server_data-before, server_frames=peer.frames_sent-frames,
                            min_window=min(peer.windows[at:]), max_window=max(peer.windows[at:]), profile=text)
                        rows.append(row)
                        print(json.dumps({k:v for k,v in row.items() if k != 'profile'}), flush=True)
                        args.output.write_text(json.dumps(dict(binaries=metadata, samples=rows), indent=2)+'\n')
        finally:
            try:
                if guest:
                    args.output.with_suffix('.log').write_bytes(guest.log)
                    guest.close()
            finally:
                peer.close()
                network_peer.Stream = BaseStream


if __name__ == '__main__':
    main()
