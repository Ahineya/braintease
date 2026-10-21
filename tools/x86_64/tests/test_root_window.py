"""Production partition selection and virtual fixture safety checks."""
import ctypes as c
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import zlib
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import partition_image as image
from gpt_image import fixture


class Identity(c.Structure):
    _fields_ = [('disk', c.c_uint8 * 16), ('partition', c.c_uint8 * 16),
                ('first', c.c_uint64), ('last', c.c_uint64), ('sector', c.c_uint)]


class Window(c.Structure):
    _fields_ = [('offset', c.c_uint64), ('bytes', c.c_uint64), ('ready', c.c_uint)]


READ = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint64, c.c_void_p, c.c_size_t)


def native_identity(data):
    return Identity((c.c_uint8 * 16).from_buffer_copy(uuid.UUID(data['disk_guid']).bytes_le),
                    (c.c_uint8 * 16).from_buffer_copy(uuid.UUID(data['partition_guid']).bytes_le),
                    data['first_lba'], data['last_lba'], data['sector_bytes'])


def seed(size=16 * 1024 * 1024):
    raw = bytearray(size)
    raw[:19] = b'RIPPLE-FUZIX-C16-V3'
    struct.pack_into('<HHI', raw, 2048, 0x7254, 10, size // 2048)
    return raw


class WindowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        lib = Path(cls.temp.name) / 'window.so'
        subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-dynamiclib' if sys.platform == 'darwin' else '-shared', '-fPIC',
                        str(HERE.parent / 'boot/gpt.c'), str(HERE.parent / 'boot/root-window.c'), '-o', str(lib)], check=True)
        cls.lib = c.CDLL(str(lib))
        cls.lib.root_window_open.argtypes = [READ, c.c_void_p, c.c_uint64, c.c_uint, c.POINTER(Identity), c.POINTER(Window)]
        cls.lib.root_window_open.restype = c.c_char_p
        cls.lib.root_window_translate.argtypes = [c.POINTER(Window), c.c_uint64, c.c_size_t, c.POINTER(c.c_uint64)]
        cls.lib.root_window_contains.argtypes = [c.POINTER(Window), c.c_uint64, c.c_uint64]

    def parse(self, data, ident, fail=None):
        violations = []
        def read(_, offset, out, length):
            if fail is not None and offset <= fail < offset + length:
                return -1
            if offset > data['bytes'] or length > data['bytes'] - offset:
                violations.append((offset, length)); return -1
            value = bytearray(length)
            for start, raw in data['spans'].items():
                a, b = max(offset, start), min(offset + length, start + len(raw))
                if a < b: value[a-offset:b-offset] = raw[a-start:b-start]
            c.memmove(out, bytes(value), length)
            return 0
        out = Window(123, 456, 1)  # Failure must revoke a previous valid result.
        error = self.lib.root_window_open(READ(read), None, data['bytes'], data['sector'],
                                          c.byref(native_identity(ident)), c.byref(out))
        self.assertEqual(violations, [])
        if error: self.assertEqual((out.offset, out.bytes, out.ready), (0, 0, 0))
        return error.decode() if error else None, out

    def sparse(self, sector, high=False, partition_bytes=16 * 1024 * 1024):
        ident = image.identity(sector)
        if high:
            ident['first_lba'] = 1 << 33
        ident['last_lba'] = ident['first_lba'] + partition_bytes // sector - 1
        data = fixture(sector, ident['last_lba'] + 4097)
        for at, raw in data['spans'].items():
            if len(raw) == 16384:
                struct.pack_into('<QQ', raw, 32, 1024 * 1024 // sector, 2 * 1024 * 1024 // sector - 1)
                raw[128:144] = image.ROOT_TYPE.bytes_le
                raw[144:160] = uuid.UUID(ident['partition_guid']).bytes_le
                struct.pack_into('<QQ', raw, 160, ident['first_lba'], ident['last_lba'])
        for raw in data['spans'].values():
            if raw[:8] == b'EFI PART':
                array = struct.unpack_from('<Q', raw, 72)[0] * sector
                struct.pack_into('<I', raw, 88, zlib.crc32(data['spans'][array]))
                struct.pack_into('<I', raw, 16, 0)
                struct.pack_into('<I', raw, 16, zlib.crc32(raw[:92]))
        data['spans'][ident['first_lba'] * sector] = seed(4096)
        # Expose the whole intended filesystem while keeping sparse metadata.
        struct.pack_into('<I', data['spans'][ident['first_lba'] * sector], 2052, 8192)
        return data, ident

    def test_native_512_4k_and_above_32bit_lba(self):
        for sector in (512, 4096):
            for high in (False, True):
                data, ident = self.sparse(sector, high)
                error, out = self.parse(data, ident)
                self.assertIsNone(error)
                self.assertEqual(out.offset, ident['first_lba'] * sector)
                self.assertEqual(out.bytes, 16 * 1024 * 1024)
                self.assertEqual(out.ready, 1)
                error, _ = self.parse(data, ident, fail=out.offset)
                self.assertEqual(error, 'root partition header read failed')

    def test_boundary_oracle_no_wraparound(self):
        rng = random.Random(123)
        for sector in (512, 4096):
            data, ident = self.sparse(sector, True)
            error, window = self.parse(data, ident)
            self.assertIsNone(error)
            offsets = [0, 1, window.bytes-1, window.bytes, window.bytes+1, (1 << 64)-1]
            lengths = [0, 1, 4096, window.bytes, (1 << 64)-1]
            pairs = [(at, n) for at in offsets for n in lengths]
            pairs += [(rng.randrange(window.bytes + 100), rng.randrange(window.bytes + 100)) for _ in range(2000)]
            for at, n in pairs:
                translated = c.c_uint64(0x12345678)
                valid = at + n <= window.bytes
                result = self.lib.root_window_translate(c.byref(window), at, n, c.byref(translated))
                self.assertEqual(result == 0, valid, (at, n))
                if valid: self.assertEqual(translated.value, window.offset + at)
                else: self.assertEqual(translated.value, 0x12345678)
                absolute = window.offset + at
                if absolute < 1 << 64:
                    self.assertEqual(bool(self.lib.root_window_contains(c.byref(window), absolute, n)), valid)
            for at in (0, window.offset-1, window.offset+window.bytes+1, (1 << 64)-1):
                self.assertFalse(self.lib.root_window_contains(c.byref(window), at, 1))
            window.ready = 0
            self.assertNotEqual(self.lib.root_window_translate(c.byref(window), 0, 0, c.byref(translated)), 0)

    def test_reserved_partition_space_stays_outside_filesystem_window(self):
        for sector in (512, 4096):
            data, ident = self.sparse(sector, high=True, partition_bytes=30 * 1024 * 1024 * 1024)
            error, window = self.parse(data, ident)
            self.assertIsNone(error)
            self.assertEqual(window.bytes, 16 * 1024 * 1024)
            out = c.c_uint64()
            self.assertNotEqual(self.lib.root_window_translate(c.byref(window), window.bytes, 1, c.byref(out)), 0)
            self.assertFalse(self.lib.root_window_contains(c.byref(window), window.offset + window.bytes, sector))
            # Large native reservations do not expand the signed C16 stream.
            struct.pack_into('<I', data['spans'][window.offset], 2052, 1024 * 1024)
            self.assertEqual(self.parse(data, ident)[0], 'root partition filesystem bounds')

    def test_fixture_identity_rejections_and_revocation(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / 'seed.img'; source.write_bytes(seed())
            ident = image.identity()
            original = directory / 'original.img'; image.create_fixture(original, source, ident)
            for case, expected in [('wrong-disk', 'root disk GUID mismatch'),
                                   ('wrong-partition', 'root partition GUID missing'),
                                   ('wrong-type', 'root partition type mismatch'),
                                   ('partition-readonly', 'root partition attributes unsupported'),
                                   ('partition-moved', 'root partition geometry mismatch'),
                                   ('root-format', 'root partition C16 V3 signature'),
                                   ('root-size', 'root partition filesystem bounds'),
                                   ('gpt-crc', 'GPT header CRC')]:
                target = directory / (case + '.img'); target.write_bytes(original.read_bytes())
                image.damage_fixture(target, ident, case)
                raw = target.read_bytes()
                error, _ = self.parse(dict(bytes=len(raw), sector=512, spans={0: raw}), ident)
                self.assertEqual(error, expected)
            data, ident = self.sparse(512)
            ident['sector_bytes'] = 4096
            self.assertEqual(self.parse(data, ident)[0], 'root partition sector identity mismatch')

    def test_identity_schema_and_creation_never_replace(self):
        image.header(image.identity())
        for key, value in [('disk_guid', str(uuid.UUID(int=0))), ('partition_guid', 'invalid'),
                           ('first_lba', True), ('last_lba', (1 << 64)-1),
                           ('sector_bytes', 513), ('version', 2), ('extra', 'bad')]:
            data = image.identity(); data[key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError): image.validate_identity(data)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / 'seed.img'; source.write_bytes(seed())
            target = directory / 'target.img'; image.create_fixture(target, source, image.identity())
            before = target.read_bytes()
            with self.assertRaises(FileExistsError): image.create_fixture(target, source, image.identity())
            self.assertEqual(target.read_bytes(), before)
            link = directory / 'link.img'; link.symlink_to(target)
            with self.assertRaises(RuntimeError): image.validate_disk(link, 512)
            with self.assertRaises(FileExistsError): image.create_fixture(link, source, image.identity())
            outside = image.outside_fingerprint(target, image.identity())
            with target.open('r+b') as out: out.seek(4 * 1024 * 1024 + 100); out.write(b'changed')
            self.assertEqual(image.outside_fingerprint(target, image.identity()), outside)
            with target.open('r+b') as out: out.seek(4 * 1024 * 1024 - 1); out.write(b'changed')
            self.assertNotEqual(image.outside_fingerprint(target, image.identity()), outside)

    def test_large_fixture_is_sparse_and_hashes_all_external_canaries(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / 'seed.img'; source.write_bytes(seed())
            target = directory / 'large.img'
            ident = image.identity(512, 30 * 1024**3)
            image.create_fixture(target, source, ident)
            self.assertEqual(target.stat().st_size, 30 * 1024**3 + 8 * 1024**2)
            self.assertLess(target.stat().st_blocks * 512, 64 * 1024**2)
            before = image.outside_fingerprint(target, ident)
            self.assertEqual(before['bytes'], 8 * 1024**2)
            with target.open('r+b') as out:
                out.seek(ident['first_lba'] * 512 + 10); out.write(b'x')
            self.assertEqual(image.outside_fingerprint(target, ident), before)
            with target.open('r+b') as out:
                out.seek((ident['last_lba'] + 1) * 512); out.write(b'x')
            self.assertNotEqual(image.outside_fingerprint(target, ident), before)


if __name__ == '__main__': unittest.main()
