#!/usr/bin/env python3
"""Check host resource decoding against independently recorded output identities."""

import shutil
import struct
import unittest
import zlib

from unpack_resources import BASE, Huffman, Reader, check, digest, inflate, numeric_tables, resources, strings, unwrap


class DecoderChecks(unittest.TestCase):
    def test_variant_stored_block_and_bounds(self):
        sample = b"host resources, not controller firmware"
        encoded = b"\x07" + struct.pack("<H", len(sample) ^ 0xBB9E) + sample
        self.assertEqual(inflate(encoded)[0], sample)
        for bad in [encoded[:-1], encoded + b"extra", b"\x01"]:
            with self.assertRaises(ValueError):
                inflate(bad)
        with self.assertRaisesRegex(ValueError, "limit"):
            inflate(encoded, limit=3)

    def test_fixed_tree_against_independent_zlib_encoder(self):
        original = b"hello hello hello hello hello"
        compressor = zlib.compressobj(wbits=-15, strategy=zlib.Z_FIXED)
        standard = compressor.compress(original) + compressor.flush()
        self.assertEqual(standard[0] & 7, 3)  # final block, standard fixed type 1
        # Preserve all payload bits; replace the standard 3-bit header with 4 bits.
        value = (int.from_bytes(standard, "little") >> 3) << 4 | 13
        variant = value.to_bytes(len(standard) + 1, "little")
        # The extra header bit sometimes still fits in the original final byte.
        if variant[-1] == 0:
            variant = variant[:-1]
        self.assertEqual(inflate(variant)[0], original)

    def test_malformed_huffman_and_strings_are_refused(self):
        with self.assertRaisesRegex(ValueError, "Oversubscribed"):
            Huffman([1, 1, 1])
        for sample in [b"\x80", b"\xff\xff\xff\xff\x7f", b"\x02a"]:
            with self.assertRaises(ValueError):
                Reader(sample).string()

    def test_wrong_resource_is_refused(self):
        with self.assertRaisesRegex(ValueError, "Unexpected leyD"):
            check("leyD", b"different bytes")

    def test_numeric_resource_requires_complete_exact_input(self):
        empty = struct.pack("<4I", 1489141855, 823301367, 682003115,
                            (158479524 + 317257137) ^ 133872560)
        self.assertEqual(numeric_tables(empty), {"int32": [], "int64": [], "float32": [], "float64": []})
        for bad in [empty[:-1], empty + b"unparsed suffix"]:
            with self.assertRaises(ValueError):
                numeric_tables(bad)


class OriginalResourceChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = BASE / "payload/Data1/playstationaccessories.dll"
        cls.openssl = shutil.which("openssl")
        if not cls.binary.exists() or not cls.openssl:
            raise unittest.SkipTest("Pinned local updater and OpenSSL required")
        outer = resources(cls.binary.read_bytes(), [("leyD", 0)])["leyD"]
        cls.outer, cls.info = unwrap(outer, cls.openssl)
        cls.image = cls.outer[40:]
        check("assembly", cls.image)
        cls.embedded = resources(cls.image, [("tSHbR", 0), ("cDKfK", 40935)])

    def test_embedded_assembly_envelope(self):
        reader = Reader(self.outer)
        self.assertEqual(reader.number("<I") ^ 153677506, 2)
        names = ["".join(chr(ord(c) ^ 3893) for c in reader.string()) for _ in range(2)]
        self.assertEqual(names, ["tSHbR", "cDKfK"])
        self.assertEqual(reader.number("<I") ^ 362300432, 43520)
        self.assertEqual(reader.pos, 40)
        self.assertEqual(self.image[:2], b"MZ")
        self.assertEqual([b["output_offset"] for b in self.info["blocks"]], [0, 32768, 43560])

    def test_string_table_identity_and_firmware_names(self):
        plain, info = unwrap(self.embedded["tSHbR"], self.openssl)
        self.assertEqual(digest(plain), "a4ba231afa857121c452a4cc9830bf1f887cb09691bd9933b64fa58ff1bda94f")
        rows = strings(plain)
        self.assertEqual(len(rows), 2560)
        self.assertEqual({r["value"] for r in rows if r["value"].startswith("FWUPDATE")},
                         {"FWUPDATE0004.bin", "FWUPDATE000B.bin", "FWUPDATE000E.bin", "FWUPDATE0044.bin"})
        # Offset-derived key agrees with the reduced original CIL expression.
        row = next(r for r in rows if r["value"] == "FWUPDATE0004.bin")
        self.assertEqual((row["offset"], row["key"]), (24890, 786))
        self.assertEqual(info["flags"], 3)

    def test_dynamic_constant_table_identity(self):
        plain, info = unwrap(self.embedded["cDKfK"], self.openssl)
        self.assertEqual((len(plain), digest(plain)),
                         (1912, "d85979383fc233974b0e5d2368ce15bb0d04148ef586037a2358aa4260191527"))
        self.assertEqual(info["flags"], 1)  # Compression only; never AES decrypt it.
        self.assertEqual([b["type"] for b in info["blocks"]], [2])

    def test_all_numeric_arrays(self):
        plain, _ = unwrap(self.embedded["cDKfK"], self.openssl)
        tables = numeric_tables(plain)
        self.assertEqual({k: len(v) for k, v in tables.items()},
                         {"int32": 349, "int64": 0, "float32": 7, "float64": 59})
        self.assertEqual(tables["int32"][6], -2144337919)
        self.assertEqual(tables["int32"][151:153], [120, 121])
        self.assertEqual(tables["float32"][0], 96.0)
        self.assertEqual(tables["float64"][12], 3.141592653589793)


if __name__ == "__main__":
    unittest.main()
