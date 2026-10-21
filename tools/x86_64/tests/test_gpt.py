"""Production GPT parser: CRCs, redundant copies, geometry and large LBAs."""
import copy
import ctypes as c
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from gpt_image import fixture, load_capture


class Partition(c.Structure):
    _fields_ = [('type', c.c_uint8 * 16), ('guid', c.c_uint8 * 16),
                ('first', c.c_uint64), ('last', c.c_uint64), ('attributes', c.c_uint64),
                ('index', c.c_uint), ('name', c.c_char * 37)]


class Info(c.Structure):
    _fields_ = [('guid', c.c_uint8 * 16), ('first', c.c_uint64), ('last', c.c_uint64),
                ('count', c.c_uint), ('sector', c.c_uint), ('partitions', Partition * 128)]


READ = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint64, c.c_void_p, c.c_size_t)


class GptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        lib = Path(cls.temp.name) / 'gpt.so'
        cmd = [os.environ.get('CC', 'cc'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-dynamiclib' if sys.platform == 'darwin' else '-shared', '-fPIC',
               str(HERE.parent / 'boot/gpt.c'), '-o', str(lib)]
        subprocess.run(cmd, check=True)
        cls.lib = c.CDLL(str(lib))
        cls.lib.gpt_discover.argtypes = [READ, c.c_void_p, c.c_uint64, c.c_uint, c.POINTER(Info)]
        cls.lib.gpt_discover.restype = c.c_char_p

    def parse(self, data, fail=None):
        violations = []
        def read(_, offset, out, length):
            if fail is not None and offset <= fail < offset + length:
                return -1
            if offset > data['bytes'] or length > data['bytes'] - offset:
                violations.append((offset, length))
                return -1
            value = bytearray(length)
            for start, raw in data['spans'].items():
                a, b = max(offset, start), min(offset + length, start + len(raw))
                if a < b:
                    value[a-offset:b-offset] = raw[a-start:b-start]
            c.memmove(out, bytes(value), length)
            return 0
        info = Info()
        error = self.lib.gpt_discover(READ(read), None, data['bytes'], data['sector'], c.byref(info))
        self.assertEqual(violations, [], "parser requested out-of-disk span")
        return error.decode() if error else None, info

    def fix(self, data, entries=False):
        spans, sector = data['spans'], data['sector']
        headers = [spans[sector], spans[data['bytes'] - sector]]
        if entries:
            for h in headers:
                at, count, size = struct.unpack_from('<QII', h, 72)
                struct.pack_into('<I', h, 88, zlib.crc32(spans[at * sector][:count * size]))
        for h in headers:
            struct.pack_into('<I', h, 16, 0)
            struct.pack_into('<I', h, 16, zlib.crc32(h[:struct.unpack_from('<I', h, 12)[0]]))

    def test_valid_512_4096_and_64_bit_lbas(self):
        for sector in (512, 4096):
            for sectors in (None, (1 << 33) + 4096):
                with self.subTest(sector=sector, sectors=sectors):
                    data = fixture(sector, sectors)
                    error, out = self.parse(data)
                    self.assertIsNone(error)
                    self.assertEqual(out.count, 2)
                    self.assertEqual(out.partitions[1].last, data['bytes'] // sector - 2049)
                    self.assertEqual(out.partitions[1].name, b'Ubuntu')
                    self.assertEqual(data, fixture(sector, sectors))

    def test_crc_damage_each_metadata_span(self):
        data = fixture()
        for at in list(data['spans'])[1:]:
            bad = copy.deepcopy(data); bad['spans'][at][0] ^= 1
            with self.subTest(at=at):
                self.assertIsNotNone(self.parse(bad)[0])

    def test_header_bounds_and_disagreement(self):
        for offset, fmt, value in [(20, '<I', 1), (24, '<Q', 2), (32, '<Q', 99),
                                   (40, '<Q', 1), (48, '<Q', 1 << 63),
                                   (72, '<Q', (1 << 64)-1), (80, '<I', 129),
                                   (84, '<I', 127), (56, '<I', 0x12345)]:
            data = fixture(); h = data['spans'][512]
            struct.pack_into(fmt, h, offset, value); self.fix(data)
            with self.subTest(offset=offset):
                self.assertIsNotNone(self.parse(data)[0])

    def test_partitions_bounds_overlap_and_duplicate_guid(self):
        for kind in ('outside', 'inverted', 'overlap', 'duplicate', 'empty_guid'):
            data = fixture()
            for at, raw in data['spans'].items():
                if len(raw) != 16384:
                    continue
                if kind == 'outside': struct.pack_into('<Q', raw, 32, 1)
                elif kind == 'inverted': struct.pack_into('<Q', raw, 32, 5000)
                elif kind == 'overlap': struct.pack_into('<Q', raw, 128+32, 3000)
                elif kind == 'duplicate': raw[128+16:128+32] = raw[16:32]
                else: raw[16:32] = bytes(16)
            self.fix(data, entries=True)
            with self.subTest(kind=kind):
                self.assertIsNotNone(self.parse(data)[0])

    def test_mbr_and_geometry(self):
        for kind in ('signature', 'hybrid', 'length', 'sector', 'short', 'unaligned'):
            data = fixture()
            if kind == 'signature': data['spans'][0][510] = 0
            elif kind == 'hybrid': data['spans'][0][466] = 0x83
            elif kind == 'length': struct.pack_into('<I', data['spans'][0], 458, 1)
            elif kind == 'sector': data['sector'] = 513
            elif kind == 'short': data['bytes'] = 512
            else: data['bytes'] -= 1
            with self.subTest(kind=kind): self.assertIsNotNone(self.parse(data)[0])

    def test_read_failures_never_succeed(self):
        data = fixture()
        for at in data['spans']:
            with self.subTest(at=at): self.assertIsNotNone(self.parse(data, fail=at)[0])

    def test_actual_hp_metadata(self):
        path = HERE / 'fixtures/hp-envy-gpt.json'
        if not path.exists(): self.skipTest('HP read-only GPT capture not present')
        error, out = self.parse(load_capture(path))
        self.assertIsNone(error); self.assertEqual(out.count, 2)
        self.assertEqual(out.partitions[0].first, 2048)
        self.assertEqual(out.partitions[1].first, 2203648)
        self.assertGreater(out.partitions[1].last, 1_900_000_000)


if __name__ == '__main__': unittest.main()
