#!/usr/bin/env python3
"""Cross-check guest algorithms against hashlib, Python integers and OpenSSL."""
import ctypes as C
import gzip
import hashlib
import os
import time
import unittest
from support import build, password_x, ige, u32, u64


class CryptoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib, _, cls.prime = build()

    def test_streaming_hashes_and_c16_integer_widths(self):
        for n in (0, 1, 55, 56, 63, 64, 65, 511, 512, 513, 16384):
            data = bytes((i * 71 + n) % 256 for i in range(n))
            for sha1 in (0, 1):
                out = C.create_string_buffer(20 if sha1 else 32)
                self.lib.tg_hash(sha1, data, n, out)
                self.assertEqual(out.raw, (hashlib.sha1 if sha1 else hashlib.sha256)(data).digest())

    def test_bounded_gzip_and_corruption(self):
        data = bytes(range(256)) * 32
        for level in (0, 1, 9):
            packed = gzip.compress(data, compresslevel=level, mtime=0)
            out = C.create_string_buffer(16384)
            self.assertEqual(self.lib.tg_gunzip(packed, len(packed), out, 16384), len(data))
            self.assertEqual(out.raw[:len(data)], data)
            self.assertEqual(self.lib.tg_gunzip(packed, len(packed), out, 128), -1)
            for bad in (packed[:-1], packed[:10], packed[:-8] + bytes([packed[-8] ^ 1]) + packed[-7:]):
                self.assertEqual(self.lib.tg_gunzip(bad, len(bad), out, 16384), -1)
        for n in range(100):
            data = os.urandom(n)
            out = C.create_string_buffer(16384)
            self.assertEqual(self.lib.tg_gunzip(data, len(data), out, 16384), -1)

    def test_srp_matches_server_proof(self):
        self.assertEqual(self.lib.tg_test_safe_prime(self.prime), 1)
        password, s1, s2 = 'pāssword'.encode(), bytes(range(16)), bytes(range(32))
        p, g = int.from_bytes(self.prime, 'big'), 3
        self.assertEqual(self.lib.tg_group(self.prime, g), 1)
        self.assertEqual(self.lib.tg_group(self.prime, 1), 0)
        self.assertEqual(self.lib.tg_group(bytes(256), g), 0)
        padded = lambda n: n.to_bytes(256, 'big')
        H = lambda data: hashlib.sha256(data).digest()
        k = int.from_bytes(H(self.prime + padded(g)), 'big')
        b = int.from_bytes(os.urandom(256), 'big')
        v = pow(g, password_x(password, s1, s2), p)
        B = padded((k*v + pow(g, b, p)) % p)
        A, proof = C.create_string_buffer(256), C.create_string_buffer(32)
        self.assertEqual(self.lib.tg_srp(password, len(password), s1, len(s1), s2, len(s2), g, self.prime, B, A, proof), 1)
        u = int.from_bytes(H(A.raw + B), 'big')
        S = pow((int.from_bytes(A.raw, 'big') * pow(v, u, p)) % p, b, p)
        expected = H(bytes(x ^ y for x, y in zip(H(self.prime), H(padded(g)))) + H(s1) + H(s2) + A.raw + B + H(padded(S)))
        self.assertEqual(proof.raw, expected)
        self.assertEqual(self.lib.tg_srp(password, len(password), s1, len(s1), s2, len(s2), g, self.prime, bytes(256), A, proof), 0)

    def test_packet_authentication_session_time_and_replay(self):
        auth, salt, session = os.urandom(256), os.urandom(8), os.urandom(8)
        H = lambda data: hashlib.sha256(data).digest()
        def packet(ident, target=session, padding=None, incoming_salt=salt,message_body=None):
            body = u32(0xdeadbeef) if message_body is None else message_body
            data = incoming_salt + target + u64(ident) + u32(0) + u32(len(body)) + body
            data += os.urandom(12 + (-(len(data)+12) % 16)) if padding is None else padding
            msg = H(auth[96:128]+data)[8:24]
            a, b = H(msg+auth[8:44]), H(auth[48:84]+msg)
            key, iv = a[:8]+b[8:24]+a[24:], b[:8]+a[8:24]+b[24:]
            return hashlib.sha1(auth).digest()[12:] + msg + ige(self.lib,data,key,iv)
        receiver = self.lib.tg_test_receiver(auth,salt,session)
        self.assertTrue(receiver)
        def receive(data):
            copy = C.create_string_buffer(data,len(data))
            return self.lib.tg_receive(receiver,copy,len(data))
        try:
            ident = int(time.time()) << 32 | 5
            valid = packet(ident)
            self.assertEqual(receive(valid),1)
            self.assertEqual(receive(valid),1)
            self.assertIn(b'seen=1',self.lib.tg_test_debug(receiver))
            self.assertEqual(receive(packet(ident+4,incoming_salt=os.urandom(8))),1)
            corrected=os.urandom(8)
            self.assertEqual(receive(packet(ident+8,message_body=u32(0xedab447b)+u64(9999996)+u32(0)+u32(48)+corrected)),1)
            self.lib.tg_test_salt.argtypes=[C.c_void_p];self.lib.tg_test_salt.restype=C.c_void_p
            self.assertEqual(C.string_at(self.lib.tg_test_salt(receiver),8),corrected)
            self.assertEqual(receive(valid[:-1]+bytes([valid[-1]^1])),0)
            self.assertEqual(receive(packet(ident+4,target=os.urandom(8))),0)
            self.assertEqual(receive(packet(ident+8,padding=bytes(12+1024))),0)
            self.assertEqual(receive(packet((int(time.time())+1000)<<32|1)),0)
            self.assertEqual(receive(packet(ident+2-1)),0)  # even server ID
        finally:
            self.lib.tg_client_free(receiver)


if __name__ == '__main__':
    unittest.main()
