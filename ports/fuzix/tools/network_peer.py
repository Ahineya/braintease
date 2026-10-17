"""Independent bounded Ethernet peer for FUZIX integration tests, not a VM backend."""
import importlib.util
from pathlib import Path
import socket
import struct
import time

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('fuzix_wire', ROOT / 'scripts/test-net-smoke.py')
wire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wire)
MASK = 0xffffffff


class Stream:
    def __init__(self, owner, local, remote, passive=False, isn=None):
        self.owner, self.local, self.remote, self.passive = owner, local, remote, passive
        self.isn = (0x12345600 + local * 123) & MASK
        self.next = self.isn + 1
        self.peer_next = None if isn is None else (isn + 1) & MASK
        self.established = self.fin = self.closed = self.reset = False
        self.closing = self.fin_sent = self.fin_acked = False
        self.pending = bytearray()
        self.received = bytearray()
        self.flight = None
        self.last_syn = 0
        self.window = 512
        self.chunk = 256
        self.progress()

    def frame(self, seq, flags=16, data=b''):
        options = b'\x02\x04\x02\x00' if flags & 2 else b''
        ack = self.peer_next or 0
        p = bytearray(struct.pack('!HHIIBBHHH', self.local, self.remote, seq & MASK, ack,
                                 (20 + len(options)) * 4, flags, 4096, 0, 0) + options + data)
        pseudo = wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 6, len(p))
        p[16:18] = struct.pack('!H', wire.checksum(pseudo + p))
        return self.owner.ip(6, p)

    def send(self, seq, flags=16, data=b''):
        self.owner.send(self.frame(seq, flags, data))

    def queue(self, data):
        self.pending.extend(data)

    def packet(self, seq, ack, flags, window, data):
        self.window = window
        if flags & 4:
            self.reset = self.closed = True
            return
        if flags & 2:
            self.peer_next = (seq + 1) & MASK
            if self.passive:
                self.send(self.isn, 18)
            else:
                assert ack == self.next, (ack, self.next)
                self.established = True
                self.send(self.next)
        elif not self.established and flags & 16 and ack == self.next:
            self.established = True
            if self.local == 2323:
                self.queue(b'\xff\xfb\x01\xff\xfb\x03\xff\xfd\x1fREMOTE_ECHO_READY\r\n')
        if self.flight and flags & 16:
            start, flight_flags, flight_data, _ = self.flight
            end = (start + len(flight_data) + bool(flight_flags & 1)) & MASK
            if ack == end:
                self.fin_acked |= bool(flight_flags & 1)
                self.flight = None
        if data:
            assert self.peer_next is not None
            overlap = (self.peer_next - seq) & MASK
            if overlap <= len(data):
                fresh = data[overlap:]
                self.received.extend(fresh)
                self.peer_next = (self.peer_next + len(fresh)) & MASK
                if self.passive and self.local == 7001:
                    self.queue(fresh)
                if self.passive and self.local == 2323 and b'CLIENT_WIRE\r' in self.received:
                    if b'CLIENT_WIRE_OK' not in self.pending and not getattr(self, 'client_replied', False):
                        self.queue(b'CLIENT_WIRE_OK\r\n')
                        self.client_replied = True
                if self.passive and self.local in self.owner.tcp_services:
                    self.owner.tcp_services[self.local](self)
            self.send(self.next)
        if flags & 1:
            end = (seq + len(data)) & MASK
            if end == self.peer_next:
                self.peer_next = (self.peer_next + 1) & MASK
                self.fin = self.closing = True
            self.send(self.next)
        self.closed = self.fin and self.fin_acked
        self.progress()

    def progress(self):
        if self.closed or self.reset or self.owner.address is None:
            return
        now = time.monotonic()
        if not self.established:
            if now - self.last_syn > 0.5:
                self.send(self.isn, 18 if self.passive else 2)
                self.last_syn = now
            return
        if self.flight:
            seq, flags, data, sent = self.flight
            if now - sent > 1:
                self.send(seq, flags, data)
                self.flight = (seq, flags, data, now)
            return
        if self.pending and self.window:
            count = min(self.chunk, self.window, len(self.pending))
            data = bytes(self.pending[:count])
            del self.pending[:count]
            self.send(self.next, 24, data)
            self.flight = (self.next, 24, data, now)
            self.next = (self.next + count) & MASK
        elif self.closing and not self.fin_sent:
            self.send(self.next, 17)
            self.flight = (self.next, 17, b'', now)
            self.next = (self.next + 1) & MASK
            self.fin_sent = True


class Peer:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(path))
        self.sock.settimeout(0.01)
        self.address = None
        self.streams = {}
        self.ping_requests = self.ping_replies = 0
        self.last_ping = 0
        self.frames = 0
        self.tcp_services = {}

    def close(self):
        self.sock.close()

    def send(self, packet):
        if self.address:
            self.sock.sendto(packet, self.address)

    def ip(self, protocol, payload):
        header = bytearray(struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 19, 0, 64,
                                       protocol, 0, wire.HOST_IP, wire.GUEST_IP))
        header[10:12] = struct.pack('!H', wire.checksum(header))
        return wire.GUEST_MAC + wire.HOST_MAC + b'\x08\x00' + header + payload

    def connect(self, port, local):
        stream = Stream(self, local, port)
        self.streams[(local, port)] = stream
        return stream

    def packet(self, frame):
        if frame == b'RNET\x01':
            self.send(b'RNET\x01' + wire.GUEST_MAC + struct.pack('!H', 1500))
            return
        if frame == b'RNET\x02':
            self.send(frame)
            return
        if frame == b'RNET\x04':
            return
        self.frames += 1
        assert self.frames < 100000, 'unexpected packet flood'
        if frame[12:14] == b'\x08\x06':
            if frame[20:22] == b'\0\x01' and frame[38:42] == wire.HOST_IP:
                self.send(wire.arp(2, wire.GUEST_MAC, wire.GUEST_MAC, wire.GUEST_IP))
            return
        if frame[12:14] != b'\x08\x00':
            return
        total = struct.unpack('!H', frame[16:18])[0]
        header = (frame[14] & 15) * 4
        assert wire.checksum(frame[14:14 + header]) == 0, 'bad guest IPv4 checksum'
        assert frame[26:30] == wire.GUEST_IP and frame[30:34] == wire.HOST_IP
        p = frame[14 + header:14 + total]
        protocol = frame[23]
        if protocol == 1:
            assert wire.checksum(p) == 0
            if p[0] == 8:
                self.ping_requests += 1
                self.send(self.ip(1, wire.echo(0, *struct.unpack('!HH', p[4:8]), p[8:])))
            elif p[0] == 0 and p[4:8] == b'\xab\xcd\0\x01':
                self.ping_replies += 1
        elif protocol == 17:
            source, target, length, checksum = struct.unpack('!HHHH', p[:8])
            assert target == 7000 and length == len(p)
            assert not checksum or wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 17, length) + p) == 0
            reply = bytearray(struct.pack('!HHHH', target, source, length, 0) + p[8:])
            check = wire.checksum(wire.HOST_IP + wire.GUEST_IP + struct.pack('!BBH', 0, 17, length) + reply)
            reply[6:8] = struct.pack('!H', check or 65535)
            self.send(self.ip(17, reply))
        elif protocol == 6:
            assert wire.checksum(frame[26:34] + struct.pack('!BBH', 0, 6, len(p)) + p) == 0
            source, target, seq, ack, offset, flags, window, _, _ = struct.unpack('!HHIIBBHHH', p[:20])
            key = (target, source)
            if key not in self.streams:
                assert flags & 2 and not flags & 16 and (target in (7001, 2323) or target in self.tcp_services), (source, target, flags)
                self.streams[key] = Stream(self, target, source, passive=True, isn=seq)
            self.streams[key].packet(seq, ack, flags, window, p[offset // 16 * 4:])

    def pump(self):
        try:
            packet, source = self.sock.recvfrom(65536)
            if self.address is None:
                assert packet == b'RNET\x01'
                self.address = source
            assert source == self.address
            self.packet(packet)
        except socket.timeout:
            pass
        for stream in list(self.streams.values()):
            stream.progress()
        # Attachment can precede a slow BIOS boot. Do not fill the emulated NIC
        # queue with probes before the guest has initialized its IP stack.
        if self.address and self.frames and not self.ping_replies and time.monotonic() - self.last_ping > 1:
            self.send(wire.arp(1, b'\xff' * 6, b'\0' * 6, wire.GUEST_IP))
            self.send(self.ip(1, wire.echo(8, 0xabcd, 1, b'host-to-fuzix\0\xff')))
            self.last_ping = time.monotonic()
