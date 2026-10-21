"""RXE1 must preserve the linked ELF load bias, including native data pointers."""
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app_link import pack_image

class Alignment(unittest.TestCase):
    def test_page_bias_and_relocated_data_with_both_metadata_parities(self):
        # Distinct aligned objects and a pointer to one, as produced by ELF.
        code = bytearray(8192)
        code[4096:4112] = bytes(range(16))
        symbols = {'entry': 32, 'riplix_helpers': 128, 'constant': 4096}
        for helpers in ([], [{'symbol': 'rcc_native_poll', 'arity': 0}]):
            meta = dict(functions=[dict(token=0x80000001, arity=1, symbol='entry')],
                        helpers=helpers, initializers=[[65537, 0x1234, 1, False]],
                        entry=0x80000001, global_bank=1, global_cells=16, errno=0)
            raw, padding = pack_image(meta, bytes(code), symbols, [(64, 4096)])
            header = struct.unpack_from('<8s14I', raw)
            _, _, total, size, nf, nh, nr, ni, *_ = header
            offset = 64 + nf*16 + nh*72 + nr*16 + ni*24
            foff = struct.unpack_from('<IIII', raw, 64)[2]
            at, addend = struct.unpack_from('<QQ', raw, 64+nf*16+nh*72)
            self.assertEqual(total, len(raw))
            self.assertEqual(size, len(raw)-offset)
            self.assertEqual((offset+padding)%4096, 0)
            self.assertEqual(offset+foff, offset+padding+32)
            self.assertEqual(offset+header[12], offset+padding+128)
            self.assertEqual(raw[offset+addend:offset+addend+16], bytes(range(16)))
            self.assertEqual(at, padding+64)
            # A loaded package at any page boundary retains aligned constants.
            for base in (0x100000, 0x4321000):
                target = base+offset+addend
                self.assertEqual(target%4096, 0)
                loaded = bytearray(raw)
                struct.pack_into('<Q', loaded, offset+at, target)
                self.assertEqual(struct.unpack_from('<Q', loaded, offset+at)[0], target)

if __name__ == '__main__': unittest.main()
