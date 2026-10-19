#!/usr/bin/env python3
"""Physical image capacity checks used by Make before BF export."""
import struct
import unittest
from check_image import BANK, INIT_CODE, APP_CODE, IMAGE_BANKS, check


class ImageChecks(unittest.TestCase):
    def test_kernel_stays_below_user_code_and_starts_after_gate(self):
        for bank, entry, count in ((BANK, 4, 1), (BANK, 4, INIT_CODE * BANK)):
            check('kernel', struct.pack('<5sHII', b'RLINK', bank, entry, count))
        for bank, entry, count in ((64000, 4, 1), (BANK, 0, 1),
                                    (BANK, 4, 0), (BANK, 4, INIT_CODE * BANK + 1)):
            with self.subTest(bank=bank, entry=entry, count=count), self.assertRaises(ValueError):
                check('kernel', struct.pack('<5sHII', b'RLINK', bank, entry, count))

    def test_init_layout_capacity_entry_and_lengths(self):
        def image(bank=BANK, code=INIT_CODE, gp=3, stack=4, entry=0, count=1, size=0):
            return struct.pack('<4s4H3I', b'SYS1', bank, code, gp, stack,
                               entry, count, size) + bytes(count * 8 + size)
        check('init', image())
        check('program', image(code=APP_CODE))
        with self.assertRaises(ValueError):
            check('program', image(code=INIT_CODE))
        check('init', image(count=IMAGE_BANKS * BANK, entry=IMAGE_BANKS * BANK - 1, size=BANK))
        for values in ({'bank': 64000}, {'code': APP_CODE}, {'gp': 1}, {'stack': 2},
                       {'entry': 1}, {'count': 0}, {'count': IMAGE_BANKS * BANK + 1}, {'size': BANK + 1}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                check('init', image(**values))
        for data in (b'', image()[:23], image()[:-1], image() + b'x'):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                check('init', data)


if __name__ == '__main__':
    unittest.main()
