"""BMP format and optional complete upstream asset preparation checks.

python3 ports/lgeneral/test_assets.py --engine PATH --data PATH
"""

import argparse
from pathlib import Path
import struct
import tempfile
import unittest

from assets import audit_paths, decode_bmp, encode_bmp, normalize_bmp, prepare_assets


ENGINE = None
DATA = None


def bitmap(width, height, bits, pixels, *, header=40, palette=b"", masks=(), compression=0):
    """Make fixtures directly from rows in file storage order."""
    extra = b"" if header > 40 else b"".join(struct.pack("<I", m) for m in masks)
    offset = 14 + header + len(extra) + len(palette)
    output = bytearray(offset + len(pixels))
    struct.pack_into("<2sIHHI", output, 0, b"BM", len(output), 0, 0, offset)
    struct.pack_into("<IiiHHIIiiII", output, 14, header, width, height, 1, bits,
                     compression, len(pixels), 0, 0, len(palette) // 4, 0)
    if header > 40:
        for index, mask in enumerate(masks):
            struct.pack_into("<I", output, 54 + 4 * index, mask)
    output[14 + header:offset] = extra + palette
    output[offset:] = pixels
    return bytes(output)


class BitmapTests(unittest.TestCase):
    def test_font_markers_and_odd_row_padding_roundtrip(self):
        # Top-left colour key plus the exact magenta/red bottom-row markers.
        rgb = bytes.fromhex("112233 010203 ffffff ff00ff ff0000 000000")
        encoded = encode_bmp(3, 2, rgb)
        self.assertEqual(decode_bmp(encoded), (3, 2, rgb))
        self.assertEqual(encoded[63:66], b"\0\0\0")
        self.assertEqual(normalize_bmp(encoded), encoded)

    def test_four_bit_palette(self):
        palette = bytes.fromhex("00000000 ff00ff00 0000ff00 33221100")
        fixture = bitmap(3, 1, 4, bytes.fromhex("12300000"), palette=palette)
        self.assertEqual(decode_bmp(fixture), (3, 1, bytes.fromhex("ff00ff ff0000 112233")))

    def test_eight_bit_top_down(self):
        palette = bytes.fromhex("0000ff00 00ff0000")
        fixture = bitmap(1, -2, 8, bytes.fromhex("00000000 01000000"), palette=palette)
        self.assertEqual(decode_bmp(fixture), (1, 2, bytes.fromhex("ff0000 00ff00")))

    def test_v4_rgb_and_v5_bitfields(self):
        rgb = bitmap(1, 1, 24, bytes.fromhex("56341200"), header=108)
        self.assertEqual(decode_bmp(rgb)[2], bytes.fromhex("123456"))
        # Explicit reverse channel order catches accidental default-BGR decoding.
        bitfields = bitmap(1, 1, 32, bytes.fromhex("12345678"), header=124,
                           masks=(0xff, 0xff00, 0xff0000), compression=3)
        self.assertEqual(decode_bmp(bitfields)[2], bytes.fromhex("123456"))

    def test_16_bit_masks_outside_v3_header(self):
        fixture = bitmap(2, 1, 16, bytes.fromhex("00f8e007"),
                         masks=(0xf800, 0x07e0, 0x001f), compression=3)
        self.assertEqual(decode_bmp(fixture)[2], bytes.fromhex("ff0000 00ff00"))

    def test_truncated_and_invalid_palette_rejected(self):
        valid = encode_bmp(2, 2, bytes(12))
        with self.assertRaisesRegex(ValueError, "truncated"):
            decode_bmp(valid[:-1])
        with self.assertRaisesRegex(ValueError, "palette"):
            decode_bmp(bitmap(1, 1, 4, bytes.fromhex("f0000000"), palette=bytes(4)))

    def test_invalid_bitfields_rejected(self):
        with self.assertRaisesRegex(ValueError, "overlapping"):
            decode_bmp(bitmap(1, 1, 32, bytes(4), masks=(255, 255, 0xff0000), compression=3))
        with self.assertRaisesRegex(ValueError, "non-contiguous"):
            decode_bmp(bitmap(1, 1, 32, bytes(4), masks=(5, 0xff00, 0xff0000), compression=3))


class PathTests(unittest.TestCase):
    def test_guest_filename_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ("a" * 31)).touch()
            with self.assertRaisesRegex(ValueError, "filename exceeds"):
                audit_paths(root)

    def test_guest_full_path_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root
            for _ in range(8):
                path = path / ("a" * 30)
                path.mkdir()
            with self.assertRaisesRegex(ValueError, "pathname exceeds"):
                audit_paths(root)


@unittest.skipUnless(ENGINE and DATA, "pass --engine and --data for complete upstream checks")
class UpstreamAssetTests(unittest.TestCase):
    def test_all_original_rgb_pixels_and_reproducible_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "data"
            report = prepare_assets(ENGINE, DATA, output)
            before = {p.relative_to(output): p.read_bytes() for p in output.rglob("*") if p.is_file()}
            self.assertEqual(report["bmp_count"], 90)
            for name, record in report["files"].items():
                if "dimensions" not in record:
                    continue
                origin = DATA if record["origin"] == "lgeneral-data-1.1.1" else ENGINE / "src"
                self.assertEqual(decode_bmp((origin / name).read_bytes()),
                                 decode_bmp((output / name).read_bytes()), name)
                self.assertEqual(struct.unpack_from("<I", (output / name).read_bytes(), 14)[0], 40)
            self.assertFalse((output / "campaigns/PG").exists())
            self.assertEqual(len(list((output / "scenarios").rglob("*"))) - 1, 23)
            self.assertTrue(report["omitted_unused_maps"])
            self.assertLessEqual(report["path_limits"]["longest_name_octets"], 30)
            self.assertTrue((output / "licenses/kukgen-LICENSE").exists())
            self.assertEqual(prepare_assets(ENGINE, DATA, output), report)
            after = {p.relative_to(output): p.read_bytes() for p in output.rglob("*") if p.is_file()}
            self.assertEqual(after, before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--data", type=Path)
    args, remaining = parser.parse_known_args()
    ENGINE, DATA = args.engine, args.data
    # skipUnless is evaluated while defining the class, before CLI parsing.
    UpstreamAssetTests.__unittest_skip__ = not (ENGINE and DATA)
    unittest.main(argv=[__file__, *remaining])
