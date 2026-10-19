#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the real C planner and generated assets; --rvm also runs C16 on JIT RVM."""
import ctypes
import hashlib
import json
import random
import shlex
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from build_theme import HERE, ROOT, Patch, Skin, artwork, build, host_environment, planner, rgb565, validate
from build_icons import build as build_icons


class SkinTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='fusede-skin-')
        cls.output = Path(cls.tmp.name)
        cls.slice = staticmethod(planner(cls.output))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_native_checks(self):
        exe = self.output / 'skincheck'
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-pedantic', str(HERE / 'skin.c'), str(HERE / 'tests/skincheck.c'), '-o', str(exe)], check=True,
                       env=host_environment())
        self.assertEqual(subprocess.check_output([str(exe)], text=True), 'FDE_SKIN_OK\n')

    def test_partition_and_corner_preservation(self):
        rng = random.Random(0xF05E)
        for _ in range(400):
            sw, sh = rng.randint(1, 50), rng.randint(1, 50)
            l, t = rng.randrange(sw), rng.randrange(sh)
            r, b = rng.randrange(sw - l), rng.randrange(sh - t)
            w, h = rng.randint(max(1, l + r), 110), rng.randint(max(1, t + b), 110)
            centre = rng.randrange(2)
            skin, patches = Skin(sw, sh, l, t, r, b, centre), (Patch * 9)()
            n = self.slice(ctypes.byref(skin), w, h, patches)
            self.assertTrue(0 <= n <= 9)
            covered = set()
            for p in patches[:n]:
                self.assertGreater(min(p.sw, p.sh, p.dw, p.dh), 0)
                self.assertLessEqual(p.sx + p.sw, sw); self.assertLessEqual(p.sy + p.sh, sh)
                self.assertLessEqual(p.dx + p.dw, w); self.assertLessEqual(p.dy + p.dh, h)
                if ((l and p.sx == 0 or r and p.sx == sw - r) and
                        (t and p.sy == 0 or b and p.sy == sh - b)):
                    self.assertEqual((p.sw, p.sh), (p.dw, p.dh))
                for y in range(p.dy, p.dy + p.dh):
                    for x in range(p.dx, p.dx + p.dw):
                        self.assertNotIn((x, y), covered)
                        covered.add((x, y))
            expected = {(x, y) for y in range(h) for x in range(w)
                        if centre or x < l or x >= w - r or y < t or y >= h - b}
            self.assertEqual(covered, expected)

    def test_invalid_input_is_atomic(self):
        for skin, w, h in [(Skin(0, 4, 0, 0, 0, 0, 1), 10, 10),
                           (Skin(4, 4, 2, 0, 2, 0, 1), 10, 10),
                           (Skin(4, 4, 65535, 0, 1, 0, 1), 10, 10),
                           (Skin(4, 4, 1, 1, 1, 1, 1), 1, 10),
                           (Skin(4, 4, 0, 0, 0, 0, 1), 0, 10),
                           (Skin(4, 4, 0, 0, 0, 0, 1), 32768, 10)]:
            patches = (Patch * 9)()
            ctypes.memset(patches, 0x5a, ctypes.sizeof(patches))
            before = bytes(patches)
            self.assertEqual(self.slice(ctypes.byref(skin), w, h, patches), -1)
            self.assertEqual(bytes(patches), before)

    def test_assets_and_determinism(self):
        theme = json.loads((HERE / 'themes/slate.json').read_text())
        output = self.output / 'assets'
        meta = build(theme, output)
        before = {p.name: hashlib.sha256(p.read_bytes()).digest() for p in output.iterdir()
                  if p.suffix not in ('.dylib', '.so')}
        build(theme, output)
        self.assertEqual(before, {p.name: hashlib.sha256(p.read_bytes()).digest() for p in output.iterdir()
                                  if p.suffix not in ('.dylib', '.so')})
        words = struct.unpack('<' + 'H' * (meta['pixel_octets'] // 2), (output / 'atlas.rgb565').read_bytes())
        masks = struct.unpack('<' + 'H' * (meta['mask_octets'] // 2), (output / 'atlas.mask').read_bytes())
        for name, asset in theme['assets'].items():
            pixels = artwork(name, asset, theme['palette'])
            x, y, w, h = meta['assets'][name]['rect']
            for yy in range(h):
                for xx in range(w):
                    p = pixels[yy * w + xx]
                    self.assertEqual(words[(y + yy) * meta['width'] + x + xx], rgb565(p[:3]))
                    word = masks[(y + yy) * meta['mask_row_words'] + (x + xx) // 16]
                    self.assertEqual(bool(word & (0x8000 >> ((x + xx) % 16))), bool(p[3]))
        self.assertLessEqual(meta['pixel_octets'] + meta['mask_octets'], 131072)

    def test_reject_malformed_metadata(self):
        original = json.loads((HERE / 'themes/slate.json').read_text())
        for change in ('colour', 'geometry', 'radius', 'metric', 'version'):
            theme = json.loads(json.dumps(original))
            if change == 'colour': theme['palette']['text'] = 'white'
            if change == 'geometry': theme['assets']['field']['insets'] = [9, 0, 9, 0]
            if change == 'radius': theme['assets']['field']['radius'] = 20
            if change == 'metric': theme['metrics']['spacing'] = -1
            if change == 'version': theme['version'] = 2
            with self.assertRaises(ValueError): validate(theme)

    def test_icon_masks_and_asset_file(self):
        output = self.output / 'icons'
        build_icons(output)
        meta = json.loads((output / 'icons.json').read_text())
        baked = json.loads((HERE / 'icons/masks.json').read_text())
        raw = (output / 'icons.mask').read_bytes()
        words = struct.unpack('<' + 'H' * (len(raw) // 2), raw)
        self.assertEqual((output / 'icons.fdi').read_bytes(), b'FDI1' +
                         struct.pack('<HHHH', meta['width'], meta['height'], 2, 0) + raw)
        for asset in meta['assets']:
            size = asset['size']; original = baked['icons'][asset['name']][str(size)]
            for y in range(size):
                for x in range(size):
                    packed = words[(asset['y'] + y) * (meta['width'] // 16) + (asset['x'] + x) // 16]
                    expected = original[y * ((size + 15) // 16) + x // 16]
                    self.assertEqual(bool(packed & (0x8000 >> ((asset['x'] + x) % 16))),
                                     bool(expected & (0x8000 >> (x % 16))))


def rvm_checks():
    with tempfile.TemporaryDirectory(prefix='fusede-c16-') as directory:
        output = Path(directory)
        objects = []
        for name, source in [('skin', HERE / 'skin.c'), ('skincheck', HERE / 'tests/skincheck.c')]:
            asm, obj = output / f'{name}.asm', output / f'{name}.pobj'
            subprocess.run([str(ROOT / 'target/release/rcc'), 'compile', str(source),
                            '-I' + str(ROOT / 'runtime/include'), '--bank-size', '64000', '-o', str(asm)], check=True)
            subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rasm'), 'assemble', str(asm),
                            '-o', str(obj), '--bank-size', '64000', '--max-immediate', '65535'], check=True, capture_output=True)
            objects.append(str(obj))
        binary = output / 'skincheck.bin'
        subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rlink'),
                        str(ROOT / 'runtime/crt0.pobj'), *objects, str(ROOT / 'runtime/libruntime.par'),
                        '-f', 'binary', '--bank-size', '64000', '-o', str(binary)], check=True, capture_output=True)
        result = subprocess.run([str(ROOT / 'target/release/rvm'), str(binary), '--bank-size', '64000',
                                 '--headless', '--jit', '--exit-code', '--timeout', '30', '--net', 'off'],
                                check=True, capture_output=True, text=True, timeout=40)
        if result.stdout.strip() != 'FDE_SKIN_OK':
            raise RuntimeError(result.stdout + result.stderr)
        print('C16 / JIT RVM: FDE_SKIN_OK')


def fuse_x_checks():
    build = ROOT / 'fuse-x/build'
    output = ROOT / 'build/fusede'
    output.mkdir(parents=True, exist_ok=True)
    program = output / 'skincheck.fxe'
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'), str(HERE / 'skin.c'),
                    str(HERE / 'tests/skincheck.c'), '--build', str(build), '-o', str(program)], check=True)
    with tempfile.TemporaryDirectory(prefix='fusede-native-') as directory:
        directory = Path(directory)
        image = directory / 'root.img'
        subprocess.run(['bun', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                        '--bootstrap-shell', '--apps', str(build / 'apps')], check=True, capture_output=True)
        subprocess.run(['bun', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image), '/bin/skincheck', str(program)],
                       check=True, capture_output=True)
        scratch = directory / 'native-code'; scratch.mkdir()
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='30', TMPDIR=str(scratch))
        result = subprocess.run([str(build / 'fuse-x'), '--run', str(image)], input='skincheck\nshutdown\n',
                                text=True, capture_output=True, timeout=40, check=True, env=env)
        if ('FDE_SKIN_OK' not in result.stdout or 'skincheck failed' in result.stdout or
                'filesystem synced; session stopped' not in result.stdout or 'panic:' in result.stdout + result.stderr):
            raise RuntimeError(result.stdout + result.stderr)
        if list(scratch.iterdir()):
            raise RuntimeError('Fuse-X loaded application code leaked after shutdown')
        # Fuse-X uses the raw root filesystem, whereas fsck.py expects the RVM
        # outer FAT disk. This private, stopped image needs no shared disk lock.
        check = ("import {checkRoot} from './ports/fuzix/tools/fsck.mjs';"
                 "import {openImage} from './ports/fuzix/tools/image.mjs';"
                 "const r=checkRoot(openImage(process.argv[1]));"
                 "if(r.issues.length)throw Error(r.issues.join('; '));"
                 "console.log('Fuse-X disposable filesystem: clean');")
        subprocess.run(['bun', '--input-type=module', '-e', check, str(image)], cwd=ROOT, check=True)
    print('Fuse-X independent .fxe / real guest exec: FDE_SKIN_OK')


if __name__ == '__main__':
    run_rvm = '--rvm' in sys.argv
    run_fuse_x = '--fuse-x' in sys.argv
    if run_rvm: sys.argv.remove('--rvm')
    if run_fuse_x: sys.argv.remove('--fuse-x')
    tests = unittest.main(exit=False)
    if not tests.result.wasSuccessful(): sys.exit(1)
    if run_rvm: rvm_checks()
    if run_fuse_x: fuse_x_checks()
