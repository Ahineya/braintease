#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the actual C rasterizer with an independent pixel sampling oracle."""
import ctypes as c
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import unittest
from build_theme import HERE, host_environment


class Pixels(c.Structure):
    _fields_ = [('pixels', c.POINTER(c.c_ushort)), ('mask', c.POINTER(c.c_ushort)),
                ('width', c.c_uint), ('height', c.c_uint)]


class Rect(c.Structure):
    _fields_ = [('x', c.c_int), ('y', c.c_int)] + [(s, c.c_uint) for s in ('sx', 'sy', 'sw', 'sh', 'width', 'height')]


Span = c.CFUNCTYPE(None, c.c_void_p, c.c_int, c.c_int, c.c_uint, c.c_ushort)


class RasterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='fde-raster-')
        library = Path(cls.tmp.name) / 'raster.so'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pedantic', '-shared', '-fPIC', str(HERE / 'raster.c'), '-o', str(library)],
                       check=True, env=host_environment())
        cls.lib = c.CDLL(str(library))
        cls.lib.fde_raster.argtypes = [c.POINTER(Pixels), c.POINTER(Rect), c.c_uint, c.c_uint, c.c_uint, c.c_ushort, Span, c.c_void_p]
        cls.lib.fde_raster.restype = c.c_int

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_sampling_clipping_mask_and_tint(self):
        rng = random.Random(253)
        for index in range(350):
            w, h = rng.randint(1, 47), rng.randint(1, 31)
            pitch = (w + 15) // 16
            values = [rng.randrange(65536) for _ in range(w * h)]
            mask = [rng.randrange(65536) for _ in range(pitch * h)]
            pixels, bits = (c.c_ushort * len(values))(*values), (c.c_ushort * len(mask))(*mask)
            mode = index % 3
            source = Pixels(pixels if mode != 2 else None, bits if mode else None, w, h)
            sx, sy = rng.randrange(w), rng.randrange(h)
            rect = Rect(rng.randint(-80, 47), rng.randint(-60, 39), sx, sy,
                        rng.randint(1, w - sx), rng.randint(1, h - sy), rng.randint(1, 97), rng.randint(1, 83))
            tint, tinted = rng.randrange(65536), index % 2
            actual = [-1] * (48 * 40); spans = []
            callback = Span(lambda _, x, y, width, colour: spans.append((x, y, width, colour)))
            self.assertEqual(self.lib.fde_raster(c.byref(source), c.byref(rect), 48, 40, tinted, tint, callback, None), 0)
            for x, y, width, colour in spans:
                self.assertTrue(0 <= x < x + width <= 48 and 0 <= y < 40)
                for xx in range(x, x + width):
                    self.assertEqual(actual[y * 48 + xx], -1)
                    actual[y * 48 + xx] = colour
            expected = [-1] * len(actual)
            for y in range(max(0, rect.y), min(40, rect.y + rect.height)):
                for x in range(max(0, rect.x), min(48, rect.x + rect.width)):
                    px = sx + (x - rect.x) * rect.sw // rect.width
                    py = sy + (y - rect.y) * rect.sh // rect.height
                    if not mode or mask[py * pitch + px // 16] & (0x8000 >> (px % 16)):
                        expected[y * 48 + x] = tint if tinted or mode == 2 else values[py * w + px]
            self.assertEqual(actual, expected, f'case {index}')

    def test_validation_and_no_output_on_failure(self):
        pixels = (c.c_ushort * 4)(1, 2, 3, 4)
        source = Pixels(pixels, None, 2, 2)
        for r in [Rect(0,0,0,0,3,2,2,2), Rect(0,0,65535,0,1,2,2,2), Rect(0,0,0,0,1,1,0,2),
                  Rect(0,0,0,0,1,1,1921,2), Rect(0,0,0,0,1,1,2,1081)]:
            spans = []
            callback = Span(lambda *values: spans.append(values))
            self.assertEqual(self.lib.fde_raster(c.byref(source), c.byref(r), 48, 40, 0, 0, callback, None), -1)
            self.assertFalse(spans)

    def test_stretched_column_and_opaque_tint_spans(self):
        pixels = (c.c_ushort * 3)(0x1234, 0x5678, 0xabcd)
        mask = (c.c_ushort * 3)(0x8000, 0, 0x8000)
        for masked, tinted in [(False,False),(True,False),(True,True),(False,True)]:
            source = Pixels(pixels, mask if masked else None, 1, 3)
            rect = Rect(-7, -2, 0, 0, 1, 3, 900, 600)
            spans=[]
            callback=Span(lambda _,x,y,width,colour: spans.append((x,y,width,colour)))
            self.assertEqual(self.lib.fde_raster(c.byref(source),c.byref(rect),640,480,tinted,0x5555,callback,None),0)
            expected=[]
            for y in range(480):
                sy=(y+2)*3//600
                if not masked or mask[sy]:
                    expected.append((0,y,640,0x5555 if tinted else pixels[sy]))
            self.assertEqual(spans,expected)

    def test_uniform_theme_row_stretch(self):
        pixels=(c.c_ushort*12)(*[0x1234]*6,*[0xabcd]*6)
        mask=(c.c_ushort*2)(0xfc00,0xfc00)
        source=Pixels(pixels,mask,6,2)
        rect=Rect(-10,-10,0,0,6,2,800,600)
        spans=[]
        callback=Span(lambda _,x,y,width,colour: spans.append((x,y,width,colour)))
        self.assertEqual(self.lib.fde_raster(c.byref(source),c.byref(rect),640,480,0,0,callback,None),0)
        self.assertEqual(spans,[(0,y,640,0x1234 if y<290 else 0xabcd) for y in range(480)])


if __name__ == '__main__':
    unittest.main()
