#!/usr/bin/env python3
"""Exercise real C engine against an independent local MTProto/SRP test peer."""
import ctypes as C
import gzip
import hashlib
import os
from pathlib import Path
import queue
import socket
import tempfile
import threading
import time
import unittest
from support import Reader, build, ige, password_x, tl, u32, u64

H = lambda p: hashlib.sha256(p).digest()
S1 = lambda p: hashlib.sha1(p).digest()
VECTOR = 0x1cb5c415


class Peer(threading.Thread):
    def __init__(self, lib, rsa, prime, corrupt=False):
        super().__init__(daemon=True)
        self.lib, self.rsa, self.prime, self.corrupt = lib, rsa, prime, corrupt
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.listener.settimeout(20)
        self.port = self.listener.getsockname()[1]
        self.errors = queue.Queue()
        self.keys = {}
        self.received = []
        self.counter = 0
        self.events = []
        self.password = 'a naïve test password'.encode()

    def read(self, n):
        result = b''
        while len(result) < n:
            data = self.connection.recv(n - len(result))
            if not data:
                raise EOFError('client disconnected')
            result += data
        return result

    def frame(self):
        n = int.from_bytes(self.read(4), 'little')
        assert 4 <= n <= 16384 and n % 4 == 0
        return self.read(n)

    def write(self, data, fragmented=False):
        data = u32(len(data)) + data
        self.events.append(('write', len(data)))
        if fragmented:
            for at in range(0, len(data), 7):
                self.connection.sendall(data[at:at+7])
                time.sleep(.0002)
        else:
            self.connection.sendall(data)
        self.events.append(('written', len(data)))

    def msgid(self):
        self.counter += 1
        return (int(time.time()) << 32) | (self.counter * 4) | 1

    def plain(self, body):
        self.write(bytes(8) + u64(self.msgid()) + u32(len(body)) + body, fragmented=True)

    def unplain(self, packet):
        r = Reader(packet)
        assert r.take(8) == bytes(8)
        assert r.integer(8) % 4 == 0
        body = Reader(r.take(r.integer()))
        r.done()
        return body

    def handshake(self, packet):
        r = self.unplain(packet)
        assert r.integer() == 0xbe7e8ef1
        nonce = r.take(16)
        r.done()
        server_nonce = os.urandom(16)
        fingerprint = bytes([self.rsa[3][0] ^ 1]) + self.rsa[3][1:] if self.corrupt else self.rsa[3]
        self.plain(u32(0x05162463) + nonce + server_nonce + tl((17*19).to_bytes(2, 'big')) + u32(VECTOR) + u32(1) + fingerprint)
        if self.corrupt:
            return None
        r = self.unplain(self.frame())
        assert r.integer() == 0xd712e4be
        assert r.take(16) == nonce and r.take(16) == server_nonce
        assert int.from_bytes(r.blob(), 'big') == 17 and int.from_bytes(r.blob(), 'big') == 19
        assert r.take(8) == self.rsa[3]
        encrypted = r.blob()
        r.done()
        rsa_plain = pow(int.from_bytes(encrypted, 'big'), self.rsa[2], self.rsa[0]).to_bytes(256, 'big')
        temp = bytes(x ^ y for x, y in zip(rsa_plain[:32], H(rsa_plain[32:])))
        data = ige(self.lib, rsa_plain[32:], temp, bytes(32), True)
        padded = data[:192][::-1]
        assert data[192:] == H(temp + padded)
        r = Reader(padded)
        assert r.integer() == 0xa9f55f95
        assert int.from_bytes(r.blob(), 'big') == 17*19
        assert int.from_bytes(r.blob(), 'big') == 17 and int.from_bytes(r.blob(), 'big') == 19
        assert r.take(16) == nonce and r.take(16) == server_nonce
        new_nonce = r.take(32)
        dc = r.integer()
        assert dc in (2, 4)
        tmp_key = S1(new_nonce+server_nonce) + S1(server_nonce+new_nonce)[:12]
        tmp_iv = S1(server_nonce+new_nonce)[12:] + S1(new_nonce+new_nonce) + new_nonce[:4]
        p = int.from_bytes(self.prime, 'big')
        exponent = int.from_bytes(os.urandom(256), 'big')
        g_a = pow(3, exponent, p).to_bytes(256, 'big')
        inner = u32(0xb5890dba) + nonce + server_nonce + u32(3) + tl(self.prime) + tl(g_a) + u32(int(time.time()))
        data = S1(inner) + inner
        data += os.urandom(-len(data) % 16)
        self.plain(u32(0xd0e8075c) + nonce + server_nonce + tl(ige(self.lib, data, tmp_key, tmp_iv)))
        r = self.unplain(self.frame())
        assert r.integer() == 0xf5045f1f
        assert r.take(16) == nonce and r.take(16) == server_nonce
        data = ige(self.lib, r.blob(), tmp_key, tmp_iv, True)
        r.done()
        inner = Reader(data[20:])
        assert inner.integer() == 0x6643b654
        assert inner.take(16) == nonce and inner.take(16) == server_nonce and inner.take(8) == bytes(8)
        gb = inner.blob()
        assert data[:20] == S1(data[20:20+inner.at])
        auth = pow(int.from_bytes(gb, 'big'), exponent, p).to_bytes(256, 'big')
        self.plain(u32(0x3bcbf734) + nonce + server_nonce + S1(new_nonce + b'\1' + S1(auth)[:8])[4:])
        salt = bytes(x ^ y for x, y in zip(new_nonce[:8], server_nonce[:8]))
        self.keys[S1(auth)[12:]] = (auth, salt)
        self.auth, self.salt = auth, salt
        return self.frame()

    def aes(self, key, x):
        a = H(key + self.auth[x:x+36])
        b = H(self.auth[40+x:76+x] + key)
        return a[:8] + b[8:24] + a[24:], b[:8] + a[8:24] + b[24:]

    def decrypt(self, packet):
        assert packet[:8] == S1(self.auth)[12:]
        key, iv = self.aes(packet[8:24], 0)
        data = ige(self.lib, packet[24:], key, iv, True)
        assert H(self.auth[88:120] + data)[8:24] == packet[8:24]
        r = Reader(data)
        salt, session, ident, seq, n = r.take(8), r.take(8), r.integer(8), r.integer(), r.integer()
        assert salt == self.salt
        assert ident % 4 == 0 and ident & 0xffffffff
        self.session = session
        body = r.take(n)
        assert 12 <= len(data) - r.at <= 1024
        return ident, seq, body

    def send(self, body, seq=1, ident=None, header_salt=None):
        self.events.append(('send', hex(int.from_bytes(body[:4], 'little'))))
        ident = self.msgid() if ident is None else ident
        data = (self.salt if header_salt is None else header_salt) + self.session + u64(ident) + u32(seq) + u32(len(body)) + body
        data += os.urandom(12 + (-(len(data)+12) % 16))
        msgkey = H(self.auth[96:128] + data)[8:24]
        key, iv = self.aes(msgkey, 8)
        self.write(S1(self.auth)[12:] + msgkey + ige(self.lib, data, key, iv), fragmented=True)

    def request(self, packet=None):
        while True:
            ident, seq, body = self.decrypt(self.frame() if packet is None else packet)
            packet = None
            r = Reader(body)
            constructor = r.integer()
            if constructor == 0x62d6b459:
                assert seq % 2 == 0
                assert r.integer() == VECTOR
                for _ in range(r.integer()):
                    r.integer(8)
                r.done()
                continue
            assert seq % 2 == 1
            if constructor == 0xda9b0d0d:
                assert r.integer() == 230 and r.integer() == 0xc1cd5ea9
                assert r.integer() == 0 and r.integer() == 12345
                for _ in range(6):
                    r.blob()
                constructor = r.integer()
            self.events.append(('request', hex(constructor)))
            return ident, seq, constructor, r

    def result(self, ident, body, compressed=False):
        if compressed:
            body = u32(0x3072cfa1) + tl(gzip.compress(body, mtime=0))
        self.send(u32(0xf35c6d01) + u64(ident) + body)

    def login(self, packet, connection):
        ident, seq, method, r = self.request(packet)
        if connection == 0:
            assert method == 0xb7e085fe
            assert r.integer() == 12345 and r.blob() == b'0'*32 and r.integer() == VECTOR and r.integer() == 0
            r.done()
            # Salt replacement must preserve the rejected query's content seqno.
            self.salt = os.urandom(8)
            self.keys[S1(self.auth)[12:]] = (self.auth, self.salt)
            self.send(u32(0xedab447b) + u64(ident) + u32(seq) + u32(48) + self.salt, seq=0)
            ident, seq2, method, r = self.request()
            assert method == 0xb7e085fe and seq == seq2
            token = os.urandom(34)
            self.expected_qr = 'tg://login?token=' + base64_url(token)
            self.result(ident, u32(0x629f1980) + u32(int(time.time())+30) + tl(token), compressed=True)
            self.approve.wait(10)
            self.send(u32(0x78d4dec1) + u32(0x564fe691) + u32(int(time.time())))
            ident, _, method, _ = self.request()
            assert method == 0xb7e085fe
            self.result(ident, u32(0x068e9916) + u32(4) + tl(b'migration-token'))
        elif connection == 1:
            assert method == 0x95ac5ce4 and r.blob() == b'migration-token'
            r.done()
            self.result(ident, u32(0x2144ca19) + u32(401) + tl(b'SESSION_PASSWORD_NEEDED'))
            ident, _, method, r = self.request()
            assert method == 0x548a30f5
            r.done()
            p = int.from_bytes(self.prime, 'big')
            pad = lambda n: n.to_bytes(256, 'big')
            s1, s2, b = os.urandom(16), os.urandom(32), int.from_bytes(os.urandom(256), 'big')
            v = pow(3, password_x(self.password, s1, s2), p)
            k = int.from_bytes(H(self.prime + pad(3)), 'big')
            B = pad((k*v + pow(3, b, p)) % p)
            parameters = (u32(0x957b50fb) + u32(12) + u32(0x3a912d4a) + tl(s1) + tl(s2) + u32(3) + tl(self.prime) +
                          tl(B) + u64(777) + tl(b'fixture hint') + u32(0xd45ab096) + u32(0x004a8537) + tl(os.urandom(32)))
            self.result(ident, parameters)
            ident, _, method, r = self.request()
            assert method == 0xd18b4d16 and r.integer() == 0xd27ff082 and r.integer(8) == 777
            A, proof = r.blob(), r.blob()
            r.done()
            u = int.from_bytes(H(A+B), 'big')
            shared = pow((int.from_bytes(A, 'big') * pow(v, u, p)) % p, b, p)
            expected = H(bytes(a ^ b for a, b in zip(H(self.prime), H(pad(3)))) + H(s1) + H(s2) + A + B + H(pad(shared)))
            assert proof == expected
            self.result(ident, u32(0x2ea2c0d4) + u32(0) + self.user())
            if hasattr(self,'account_loop'):
                self.account_loop(False)
                return
            ident, _, method, r = self.request()
            assert method == 0xfef48f62 and r.integer() == 2 and r.integer() == 0x7da07ec9
            self.received.append(r.blob())
            assert r.take(8) != bytes(8)
            r.done()
            self.result(ident, u32(0x9015e101) + u32(2) + u32(1) + u32(1) + u32(1) + u32(int(time.time())))
            self.sent.wait(10)
        else:
            assert method == 0x0d91a548 and r.integer() == VECTOR and r.integer() == 1 and r.integer() == 0xf7c1b13f
            r.done()
            self.result(ident, u32(VECTOR) + u32(1) + self.user())
            if hasattr(self,'account_loop'):
                self.account_loop(True)
                return
            self.restored.wait(10)

    @staticmethod
    def user():
        return u32(0xb1b8cc83) + u32(0) + u32(0) + u64(42)

    def run(self):
        self.approve, self.sent, self.restored = threading.Event(), threading.Event(), threading.Event()
        try:
            for connection in range(1 if self.corrupt else 3):
                self.connection, _ = self.listener.accept()
                self.events.append(('accepted', connection))
                self.connection.settimeout(15)
                self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                assert self.read(4) == b'\xee'*4
                packet = self.frame()
                if packet[:8] == bytes(8):
                    packet = self.handshake(packet)
                    if self.corrupt:
                        self.connection.close()
                        return
                else:
                    self.auth, self.salt = self.keys[packet[:8]]
                self.login(packet, connection)
                # Flush the migration result before closing a connection that
                # can still have unconsumed client acknowledgements. A plain
                # close with unread data can reset TCP and discard its tail.
                self.connection.shutdown(socket.SHUT_WR)
                try:
                    while self.connection.recv(4096):
                        pass
                except ConnectionResetError:
                    pass
                self.connection.close()
        except BaseException as error:
            self.errors.put(error)
        finally:
            self.listener.close()


def base64_url(data):
    import base64
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode()


class LoginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib, cls.rsa, cls.prime = build()

    def drive(self, client, wanted, peer, timeout=15):
        until = time.monotonic()+timeout
        states = []
        while time.monotonic() < until:
            if not peer.errors.empty():
                raise peer.errors.get()
            self.lib.tg_client_step(client)
            debug = self.lib.tg_test_debug(client).decode()
            if not states or states[-1] != debug:
                states.append(debug)
            state = self.lib.tg_client_state(client)
            if state == wanted:
                return
            if state == 6:
                time.sleep(.02)
                if not peer.errors.empty():
                    raise peer.errors.get()
                self.fail(self.lib.tg_client_status(client).decode())
            time.sleep(.002)
        import sys, traceback
        stack = ''.join(traceback.format_stack(sys._current_frames()[peer.ident])) if peer.ident in sys._current_frames() else 'peer finished'
        self.fail('State timeout: ' + self.lib.tg_client_status(client).decode() + ' ' + repr(peer.events) + ' ' + repr(states[-15:]) + '\n' + stack)

    def test_qr_migration_srp_send_and_session_restore(self):
        peer = Peer(self.lib, self.rsa, self.prime)
        peer.password = 'пароль é🙂'.encode()
        peer.start()
        with tempfile.TemporaryDirectory(prefix='tg-fixture-') as folder:
            cfg, session = Path(folder)/'config', Path(folder)/'session'
            cfg.write_text('api_id=12345\napi_hash='+'0'*32+'\n')
            cfg.chmod(0o600)
            client = self.lib.tg_test_client(os.fsencode(cfg), os.fsencode(session), peer.port)
            self.assertTrue(client)
            try:
                self.drive(client, 2, peer)
                self.assertEqual(self.lib.tg_client_qr(client).decode(), peer.expected_qr)
                peer.approve.set()
                self.drive(client, 3, peer)
                self.assertEqual(self.lib.tg_client_hint(client), b'fixture hint')
                for scalar in [0, 27, 0xd800, 0x110000]:
                    self.assertEqual(self.lib.tg_client_password_scalar(client, scalar), 0)
                for _ in range(511):
                    self.assertEqual(self.lib.tg_client_password_key(client, ord('x')), 1)
                self.assertEqual(self.lib.tg_client_password_scalar(client, ord('🙂')), 0)
                self.assertEqual(self.lib.tg_client_password_length(client), 511)
                self.assertEqual(self.lib.tg_client_password_key(client, 21), 1)
                # Secret input stays in erasable C storage. Backspace removes
                # complete graphemes, including a combining sequence and emoji.
                for text in ['é', '👩\u200d💻', 'Ж']:
                    for scalar in text:
                        self.assertEqual(self.lib.tg_client_password_scalar(client, ord(scalar)), 1)
                    self.assertEqual(self.lib.tg_client_password_key(client, 127), 1)
                    self.assertEqual(self.lib.tg_client_password_length(client), 0)
                for scalar in peer.password.decode():
                    self.assertEqual(self.lib.tg_client_password_scalar(client, ord(scalar)), 1)
                self.assertEqual(self.lib.tg_client_password_length(client), len(peer.password))
                self.lib.tg_client_password_key(client, 10)
                self.assertEqual(self.lib.tg_client_password_length(client), 0)
                self.drive(client, 4, peer)
                self.assertEqual(session.stat().st_mode & 0o777, 0o600)
                self.assertEqual(session.stat().st_size, 328)
                message = b'Hello from the isolated C16 Telegram fixture'
                self.assertEqual(self.lib.tg_client_send(client, message, len(message)), 1)
                self.drive(client, 4, peer)
                self.assertEqual(peer.received, [message])
                peer.sent.set()
            finally:
                self.lib.tg_client_free(client)
            client = self.lib.tg_test_client(os.fsencode(cfg), os.fsencode(session), peer.port)
            self.assertTrue(client)
            try:
                self.drive(client, 4, peer)
                self.assertIn(b'Session restored', self.lib.tg_client_status(client))
                peer.restored.set()
            finally:
                self.lib.tg_client_free(client)
        peer.join(5)
        self.assertFalse(peer.is_alive())
        if not peer.errors.empty():
            raise peer.errors.get()

    def test_untrusted_rsa_anchor_stops_before_credentials(self):
        peer = Peer(self.lib, self.rsa, self.prime, corrupt=True)
        peer.start()
        with tempfile.TemporaryDirectory(prefix='tg-fixture-') as folder:
            cfg = Path(folder)/'config'
            cfg.write_text('api_id=12345\napi_hash='+'0'*32+'\n')
            cfg.chmod(0o600)
            client = self.lib.tg_test_client(os.fsencode(cfg), os.fsencode(Path(folder)/'session'), peer.port)
            try:
                self.drive(client, 6, peer)
                self.assertFalse((Path(folder)/'session').exists())
            finally:
                self.lib.tg_client_free(client)
        peer.join(5)


if __name__ == '__main__':
    unittest.main()
