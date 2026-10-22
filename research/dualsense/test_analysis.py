#!/usr/bin/env python3
"""Synthetic controls and independent corpus checks for the offline analysis."""
import gzip
import json
from pathlib import Path
import struct
import unittest
import zlib

from analyze import (ROOT, compare, compression_probe, decode_firmware_report,
                     entropy, header, regions, vector_candidates, without_uniform_tail)
from fetch import catalog, verify


class SyntheticControls(unittest.TestCase):
    def test_entropy_controls(self):
        self.assertEqual(entropy(b"\0" * 4096), 0)
        self.assertEqual(entropy(bytes(range(256)) * 16), 8)

    def test_real_compressed_streams_and_false_magic(self):
        original = b"firmware compression control\n" * 100
        for mode, packed in [("gzip", gzip.compress(original, mtime=0)),
                             ("zlib", zlib.compress(original))]:
            results = compression_probe(b"prefix!" + packed)["validated_zlib_gzip_streams"]
            self.assertTrue(any(r["offset"] == 7 and r["format"] == mode and
                                r["output_bytes"] == len(original) for r in results))
        fake = compression_probe(b"junk\x1f\x8b\x08not a compressed stream")
        self.assertEqual(fake["signature_hits_only"]["gzip"], [4])
        self.assertEqual(fake["validated_zlib_gzip_streams"], [])

    def test_vector_table_at_nonzero_word_offset(self):
        words = [0x20010000, 0x10000101, 0x10000121, 0x10000141,
                 0x10000161, 0x10000181, 0x100001A1, 0, 0, 0, 0,
                 0x100001C1, 0x100001E1, 0, 0x10000201, 0x10000221]
        data = b"\xff" * 12 + struct.pack("<16I", *words)
        self.assertEqual(vector_candidates(data), [{"offset": 12, "stack": words[0], "reset": words[1]}])
        words[7] = 1
        self.assertEqual(vector_candidates(struct.pack("<16I", *words)), [])

    def test_uniform_tail_and_shifted_shared_blocks(self):
        self.assertEqual(without_uniform_tail(b"hello" + b"\xff" * 64), b"hello")
        self.assertEqual(without_uniform_tail(b"hello" + b"\xff" * 63), b"hello" + b"\xff" * 63)
        a, b = b"0123456789abcdef", b"fedcba9876543210"
        result = compare(a + b, b + a)
        self.assertEqual(result["equal_aligned_16byte_blocks"], 0)
        self.assertEqual(result["shared_nonuniform_aligned_16byte_values"], 2)
        self.assertEqual(compare(b"same", b"same")["longest_equal_run"], 4)
        self.assertEqual(compare(b"aaaa", b"bbbb")["equal_bytes"], 0)

    def test_reject_short_reports_and_wrong_headers(self):
        with self.assertRaises(ValueError):
            decode_firmware_report(b"\x20")
        with self.assertRaises(ValueError):
            header(b"\0" * 256)


class CorpusChecks(unittest.TestCase):
    def test_every_image_digest_and_region_length_relationship(self):
        _, entries = catalog()
        for entry in entries:
            with self.subTest(file=entry["file"]):
                data = (ROOT / entry["file"]).read_bytes()
                verify(data, entry)
                with self.assertRaises(ValueError):
                    verify(data[:-1], entry)
                corrupted = bytearray(data)
                corrupted[0x100] ^= 1
                with self.assertRaises(ValueError):
                    verify(corrupted, entry)
                rs = regions(data)
                self.assertEqual([r["offset"] for r in rs], [0, 0x4F000, 0xAF000])
                self.assertEqual(header(data)["word_68_region_size_candidate"], rs[0]["size"])
                for r in rs[1:]:
                    self.assertEqual(r["unexplained_overhead_above_length_field"], 0xC0)
                    self.assertEqual(r["secondary_update_version"], int(entry["version"], 16))

    def test_controller_matches_one_catalog_header(self):
        report = json.loads((ROOT / "controller.json").read_text())
        decoded = decode_firmware_report(bytes.fromhex(report["feature_report_20_hex"]))
        self.assertEqual(decoded["hardware_version"], 0x616)
        self.assertEqual(decoded["internal_firmware_version"], 0x0110002A)
        self.assertEqual(decoded["update_version"], 0x0630)
        _, entries = catalog()
        matched = []
        for entry in entries:
            h = header((ROOT / entry["file"]).read_bytes())
            if all(h[k] == decoded[k] for k in
                   ("build_date", "build_time", "internal_firmware_version", "update_version")):
                matched.append(entry["file"])
        self.assertEqual(matched, ["firmware/dualsense/type-0004/0x0630/FWUPDATE0004.bin"])


if __name__ == "__main__":
    unittest.main()
