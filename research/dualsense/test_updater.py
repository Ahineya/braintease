#!/usr/bin/env python3
"""Offline checks for updater archive decoding and source preservation."""

import hashlib
from pathlib import Path
import random
import struct
import tempfile
import unittest
import zlib

import extract_updater as updater


def encoded_member(plain, name):
    # Independent byte-by-byte encoder of the documented ISx transformation.
    seed = name.encode()
    key = [v ^ (0x13, 0x35, 0x86, 0x07)[i % 4] for i, v in enumerate(seed)]
    encoded = bytearray()
    for i, value in enumerate(zlib.compress(plain)):
        value = key[(i % 1024) % len(key)] ^ (~value & 255)
        encoded.append(((value << 4) | (value >> 4)) & 255)
    return bytes(encoded)


def archive(name="hello.bin"):
    data = bytearray(0x200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<II", data, 0x98 + 16, 0x100, 0x100)
    header = struct.pack("<14sHI", b"ISSetupStream\0", 1, 4) + bytes(26)
    name_bytes = name.encode("utf-16-le") + b"\0\0"
    payload = encoded_member(b"test", name)
    member = struct.pack("<II2sI8sH", len(name_bytes), 6, b"\0\0", len(payload), bytes(8), 1)
    return bytes(data) + header + member + bytes(24) + name_bytes + payload


class UpdaterTests(unittest.TestCase):
    def test_decoder_crosses_block_and_chunk_boundaries(self):
        plain = random.Random(123).randbytes(updater.CHUNK + 12345)
        name = "PlayStation® Accessories.msi"
        encoded = encoded_member(plain, name)
        self.assertGreater(len(encoded), updater.CHUNK)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "output"
            result = updater.decode(encoded, name, path)
            self.assertEqual(path.read_bytes(), plain)
            self.assertEqual(result["sha256"], hashlib.sha256(plain).hexdigest())

    def test_parser_rejects_truncation_and_unsafe_names(self):
        self.assertEqual(updater.members(archive())[0][0], "hello.bin")
        for name in ["../escape", "..\\escape", "C:\\escape", "a/b", "a\\b", "."]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                updater.members(archive(name))
        for end in [0, 63, 0x90, 0x210, len(archive()) - 1]:
            with self.subTest(end=end), self.assertRaises(ValueError):
                updater.members(archive()[:end])

    def test_decoder_rejects_missing_checksum_and_trailing_data(self):
        payload = encoded_member(b"hello", "hello.bin")
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                updater.decode(payload[:-1], "hello.bin", Path(tmp) / "truncated")
            with self.assertRaises(ValueError):
                updater.decode(payload + b"junk", "hello.bin", Path(tmp) / "trailing")

    def test_unknown_installer_is_not_extracted(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / "input", Path(tmp) / "out"
            source.write_bytes(archive())
            with self.assertRaisesRegex(ValueError, "pinned"):
                updater.extract(source, output)
            self.assertFalse(output.exists())

    def test_pinned_installer_when_present(self):
        root = Path(__file__).resolve().parent / "analysis/followup"
        source = root / "PlayStationAccessoriesInstaller.exe"
        if not source.exists():
            self.skipTest("Official installer not downloaded")
        data = source.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), updater.INSTALLER_SHA256)
        self.assertEqual(len(updater.members(data)), 66)
        name, offset, size = next(x for x in updater.members(data) if x[0].endswith(".msi"))
        with tempfile.TemporaryDirectory() as tmp:
            result = updater.decode(data[offset:offset + size], name, Path(tmp) / "sony.msi")
        self.assertEqual(result["sha256"], "15d8f3f380ad689f3df76c45baffa8c527949368e24dedfaa6d1469285ee2b4b")
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), updater.INSTALLER_SHA256)


if __name__ == "__main__":
    unittest.main()
