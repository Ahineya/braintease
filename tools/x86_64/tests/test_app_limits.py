"""The x64 RXE1 limit covers the complete package, including alignment/tables."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app_link import IMAGE_MAX, flatten, pack_image


def metadata(functions=1):
    return dict(functions=[dict(token=0x80000000 | (i+1), arity=1, symbol='entry')
                           for i in range(functions)], helpers=[], initializers=[],
                entry=0x80000001, global_bank=1, global_cells=16, errno=0)


class Limits(unittest.TestCase):
    def test_total_limit_includes_tables_and_bias_for_both_alignment_edges(self):
        self.assertEqual(IMAGE_MAX, 32 * 1024 * 1024)
        for count in (1, 252, 253):
            with self.subTest(functions=count):
                meta = metadata(count)
                tables = 64 + count*16
                bias = (-tables) % 4096
                room = IMAGE_MAX - tables - bias
                code = bytes(room)
                raw, padding = pack_image(meta, code, {'entry': 0, 'riplix_helpers': 0}, [])
                header = struct.unpack_from('<8s14I', raw)
                self.assertEqual(len(raw), IMAGE_MAX)
                self.assertEqual(header[2], IMAGE_MAX)
                self.assertEqual(header[3], room + bias)
                self.assertEqual(padding, bias)
                self.assertEqual((tables+padding) % 4096, 0)
                # Both raw code and padded code fit the limit: only total
                # package accounting catches this former builder mismatch.
                self.assertLessEqual(room + 1 + bias, IMAGE_MAX)
                with self.assertRaisesRegex(RuntimeError, 'package including metadata and alignment'):
                    pack_image(meta, code + b'X', {'entry': 0, 'riplix_helpers': 0}, [])

    def test_oversized_metadata_rejected_before_table_construction(self):
        class LargeTable:
            def __len__(self):
                return IMAGE_MAX // 16 + 1

            def __iter__(self):
                raise AssertionError('oversized table must not be iterated')

        meta = metadata()
        meta['functions'] = LargeTable()
        with self.assertRaisesRegex(RuntimeError, 'package including metadata and alignment'):
            pack_image(meta, b'X', {}, [])

    def test_load_span_rejected_before_allocation_above_the_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'app.elf'
            # A minimal synthetic ELF PIE with one file-empty PT_LOAD span.
            # flatten must also count memory-only tail bytes, not just filesize.
            elf = bytearray(64+56)
            elf[:6] = b'\x7fELF\x02\x01'
            struct.pack_into('<HH', elf, 16, 3, 62)
            struct.pack_into('<Q', elf, 32, 64)
            struct.pack_into('<HH', elf, 54, 56, 1)
            for size in (IMAGE_MAX, IMAGE_MAX+1):
                struct.pack_into('<IIQQQQQQ', elf, 64, 1, 5, 0, 0, 0, 0, size, 4096)
                path.write_bytes(elf)
                if size == IMAGE_MAX:
                    code, symbols, relocations = flatten(path)
                    self.assertEqual(len(code), IMAGE_MAX)
                    self.assertEqual(symbols, {})
                    self.assertEqual(relocations, [])
                else:
                    with self.assertRaisesRegex(RuntimeError, '32 MiB load span'):
                        flatten(path)


if __name__ == '__main__':
    unittest.main()
