#!/usr/bin/env python3
"""ANSI sink recovery when suspend cleanup interrupts an unfinished repaint."""
import unittest
from guest import Screen


class ScreenTest(unittest.TestCase):
    def test_escape_aborts_partial_csi_across_reads(self):
        screen = Screen()
        screen.feed(b'\x1b[14;')
        screen.feed(b'\x1b[0m\x1b(B\x1b[Hrestored')
        self.assertTrue(screen.text().startswith('restored'))
        self.assertEqual(screen.pending, '')


if __name__ == '__main__':
    unittest.main()
