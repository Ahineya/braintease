#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build original FuseDE artwork: PNG preview, C16 atlas, metadata and C arrays.

Only the Python standard library and a host C compiler are required. Gradients
and bevels are authored here at their final pixel size; the JSON is the palette
and metrics source. No guest image decoder, vector engine or font bake is needed.
"""
import argparse
import binascii
import ctypes
import json
import re
import shlex
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]


class Skin(ctypes.Structure):
    _fields_ = [(s, ctypes.c_uint) for s in ('width', 'height', 'left', 'top', 'right', 'bottom', 'centre')]


class Patch(ctypes.Structure):
    _fields_ = [(s, ctypes.c_uint) for s in ('sx', 'sy', 'sw', 'sh', 'dx', 'dy', 'dw', 'dh')]


def host_environment():
    env = dict(os.environ)
    # Keep Apple's compiler and SDK in the same selected toolchain. On systems
    # with both Xcode and Command Line Tools, clang's implicit SDK can differ.
    if sys.platform == 'darwin' and 'SDKROOT' not in env:
        env['SDKROOT'] = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    return env


def planner(output):
    """Use the actual C planner for every rectangle in the reference preview."""
    library = output / ('skin.dylib' if sys.platform == 'darwin' else 'skin.so')
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-pedantic', '-shared', '-fPIC',
                    str(HERE / 'skin.c'), '-o', str(library)], check=True, env=host_environment())
    lib = ctypes.CDLL(str(library))
    lib.fde_slice.argtypes = [ctypes.POINTER(Skin), ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(Patch)]
    lib.fde_slice.restype = ctypes.c_int
    return lib.fde_slice


def rgb(value):
    if not isinstance(value, str) or not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
        raise ValueError(f'invalid colour: {value!r}')
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


def rgb565(colour):
    r, g, b = colour
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def unpack565(value):
    r, g, b = value >> 11, value >> 5 & 63, value & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def mix(a, b, n, d):
    if d <= 0:
        return a
    return tuple((x * (d - n) + y * n + d // 2) // d for x, y in zip(a, b))


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', binascii.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + bytes(c for pixel in pixels[y * width:(y + 1) * width] for c in pixel)
                    for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def validate(theme):
    if theme['version'] != 1 or not isinstance(theme['name'], str):
        raise ValueError('unsupported theme version/name')
    for colour in theme['palette'].values():
        rgb(colour)
    for name, value in theme['metrics'].items():
        if type(value) is not int or not 1 <= value <= 64:
            raise ValueError(f'invalid metric {name}')
    if not 1 <= len(theme['assets']) <= 32:
        raise ValueError('expected 1..32 assets')
    for name, asset in theme['assets'].items():
        if not re.fullmatch(r'[a-z][a-z0-9-]*', name):
            raise ValueError(f'invalid asset name: {name}')
        w, h = asset['size']
        l, t, r, b = asset['insets']
        if any(type(n) is not int for n in (w, h, l, t, r, b, asset['radius'])):
            raise ValueError(f'non-integer geometry: {name}')
        if not (1 <= w <= 256 and 1 <= h <= 256 and min(l, t, r, b) >= 0 and
                l + r < w and t + b < h and 0 <= asset['radius'] <= min(l, t, r, b)):
            raise ValueError(f'invalid nine-slice geometry: {name}')
        rgb(asset['top']); rgb(asset['bottom'])


def artwork(name, asset, palette):
    w, h = asset['size']
    radius = asset['radius']
    def inside(x, y, inset=0):
        ww, hh, rr = w - 2 * inset, h - 2 * inset, max(0, radius - inset)
        x -= inset; y -= inset
        if not (0 <= x < ww and 0 <= y < hh):
            return False
        xx, yy = min(x, ww - 1 - x), min(y, hh - 1 - y)
        return xx >= rr or yy >= rr or (rr - xx - 0.5) ** 2 + (rr - yy - 0.5) ** 2 <= rr * rr
    pixels = []
    top, bottom = rgb(asset['top']), rgb(asset['bottom'])
    for y in range(h):
        for x in range(w):
            if not inside(x, y):
                pixels.append((0, 0, 0, 0)); continue
            window = name.startswith('window-')
            title = asset['insets'][1]
            colour = mix(top, bottom, min(y, title - 1) if window else y, title - 1 if window else h - 1)
            if window and y >= title:
                colour = rgb(palette['surface'])
                if x < 4 or x >= w - 4 or y >= h - 4:
                    colour = bottom
            if name == 'field' and 2 <= x < w - 2 and 2 <= y < h - 2:
                colour = rgb(palette['field'])
            if not inside(x, y, 1):
                colour = rgb(palette['outline'])
            elif not inside(x, y, 2):
                colour = mix(colour, rgb(palette['highlight']) if y < h // 2 else rgb(palette['outline']), 1, 2)
            # Quantize the preview too: the appearance includes RGB565 banding.
            pixels.append((*unpack565(rgb565(colour)), 255))
    return pixels


def draw(slice_fn, canvas, cw, ch, asset, pixels, x, y, width, height, centre=True):
    sw, sh = asset['size']
    skin = Skin(sw, sh, *asset['insets'], int(centre))
    patches = (Patch * 9)()
    count = slice_fn(ctypes.byref(skin), width, height, patches)
    if count < 0:
        raise ValueError(f'destination too small: {width}x{height}')
    for p in patches[:count]:
        for yy in range(p.dh):
            py = y + p.dy + yy
            if not 0 <= py < ch:
                continue
            sy = p.sy + yy * p.sh // p.dh
            for xx in range(p.dw):
                px = x + p.dx + xx
                if 0 <= px < cw:
                    pixel = pixels[sy * sw + p.sx + xx * p.sw // p.dw]
                    if pixel[3]:
                        canvas[py * cw + px] = pixel


def study(canvas, theme, output):
    """A static layout study using the existing guest ROM font, not host fonts.

    These labels/icons are authoring illustrations, not functioning widgets.
    The font remains under its existing upstream license, copied beside output.
    """
    source = (ROOT / 'ports/graphics/fonts/rom8x16.inc').read_text()
    font = [int(n, 16) for n in re.findall(r'0x[0-9a-fA-F]+', source.split('{', 1)[1])]
    if len(font) != 4096:
        raise ValueError('unexpected ROM font geometry')
    def text(x, y, value, colour='text'):
        pixel = (*unpack565(rgb565(rgb(theme['palette'][colour]))), 255)
        for char in value:
            for yy in range(16):
                for xx in range(8):
                    if font[ord(char) * 16 + yy] & (0x8000 >> xx) and 0 <= x + xx < 640 and 0 <= y + yy < 480:
                        canvas[(y + yy) * 640 + x + xx] = pixel
            x += 8
    text(12, 4, 'FuseDE'); text(80, 4, 'Applications'); text(200, 4, 'Places'); text(280, 4, 'Session')
    text(560, 4, '14:32', 'muted')
    text(450, 78, 'Terminal', 'muted'); text(446, 110, 'fuzix# ls', 'muted')
    text(446, 130, 'bin dev etc home', 'muted'); text(446, 150, 'mnt tmp usr', 'muted')
    text(446, 190, 'fuzix# _', 'muted')
    text(46, 54, 'Files - Home'); text(354, 54, '- [] x')
    text(60, 93, '/home/user', 'muted'); text(60, 133, 'Name', 'muted'); text(362, 133, 'Size', 'muted')
    for index, (name, size) in enumerate([('Projects', '<DIR>'), ('Documents', '<DIR>'), ('Pictures', '<DIR>'),
                                          ('hello.rfl', '1.2K'), ('notes.txt', '842B')]):
        yy = 157 + index * 24
        text(61, yy, '+' if index < 3 else '-'); text(80, yy, name); text(354, yy, size, 'muted')
    text(74, 357, 'Back'); text(172, 357, 'New'); text(352, 357, 'Open')
    text(48, 402, '5 items - Projects selected', 'muted')
    for index, label in enumerate(['F', '>_', 'R', '12', 'T', '*']):
        text(188 + index * 48, 439, label)
    png(output / 'study.png', 640, 480, canvas)
    (output / 'preview-font-license.txt').write_text((ROOT / 'ports/graphics/fonts/LICENSE').read_text())


def build(theme, output):
    validate(theme)
    output.mkdir(parents=True, exist_ok=True)
    slice_fn = planner(output)
    # A narrow shelf atlas: two-pixel gutters prevent neighbouring assets from
    # being sampled by accident. Mask words are MSB-first, serialized LE16.
    aw = max(a['size'][0] for a in theme['assets'].values()) + 2
    ah = sum(a['size'][1] + 2 for a in theme['assets'].values())
    if aw * ah * 2 + ((aw + 15) // 16) * ah * 2 > 131072:
        raise ValueError('theme exceeds the 128 KiB source atlas budget')
    atlas = [(0, 0, 0, 0)] * (aw * ah)
    meta = {'version': 1, 'name': theme['name'], 'pixel_format': 'RGB565', 'file_order': 'little-endian-octets',
            'width': aw, 'height': ah, 'mask_row_words': (aw + 15) // 16, 'mask_bit_order': 'msb-first',
            'palette': theme['palette'], 'metrics': theme['metrics'], 'assets': {}}
    sources = {}
    y = 1
    for name, asset in theme['assets'].items():
        w, h = asset['size']; pixels = artwork(name, asset, theme['palette'])
        sources[name] = pixels
        png(output / f'{name}.png', w, h, pixels)
        for yy in range(h):
            atlas[(y + yy) * aw + 1:(y + yy) * aw + 1 + w] = pixels[yy * w:(yy + 1) * w]
        meta['assets'][name] = {'rect': [1, y, w, h], 'insets': asset['insets'], 'centre': True}
        y += h + 2
    words = [rgb565(p[:3]) for p in atlas]
    mask = []
    for yy in range(ah):
        for base in range(0, aw, 16):
            mask.append(sum(1 << (15 - bit) for bit in range(min(16, aw - base)) if atlas[yy * aw + base + bit][3]))
    png(output / 'atlas.png', aw, ah, atlas)
    (output / 'atlas.rgb565').write_bytes(struct.pack('<' + 'H' * len(words), *words))
    (output / 'atlas.mask').write_bytes(struct.pack('<' + 'H' * len(mask), *mask))
    (output / 'theme.fdi').write_bytes(b'FDI1' + struct.pack('<HHHH', aw, ah, 1, 0) +
                                      struct.pack('<' + 'H' * len(words), *words) +
                                      struct.pack('<' + 'H' * len(mask), *mask))
    meta['pixel_octets'], meta['mask_octets'] = len(words) * 2, len(mask) * 2
    (output / 'theme.json').write_text(json.dumps(meta, indent=2) + '\n')
    arrays = ['/* Generated original theme artwork; see ports/fusede/themes/README.md. */',
              '#ifndef FDE_THEME_DATA_H', '#define FDE_THEME_DATA_H', '#include "fde_skin.h"',
              f'#define FDE_THEME_WIDTH {aw}', f'#define FDE_THEME_HEIGHT {ah}']
    for name, colour in theme['palette'].items():
        r, g, b = rgb(colour)
        arrays.append(f'#define FDE_COLOUR_{name.upper()} GR_RGB({r},{g},{b})')
    for index, name in enumerate(meta['assets']):
        arrays.append(f'#define FDE_ASSET_{name.upper().replace("-", "_")} {index}')
    arrays.append(f'static const struct fde_asset fde_theme_assets[{len(meta["assets"])}] = {{')
    for asset in meta['assets'].values():
        x, y, w, h = asset['rect']; l, t, r, b = asset['insets']
        arrays.append(f'    {{{x},{y},{{{w},{h},{l},{t},{r},{b},1}}}},')
    arrays.append('};')
    for name, values in [('pixels', words), ('mask', mask)]:
        arrays.append(f'static const unsigned short fde_theme_{name}[{len(values)}] = {{')
        arrays.extend('    ' + ','.join(f'0x{v:04x}' for v in values[i:i + 12]) + ',' for i in range(0, len(values), 12))
        arrays.append('};')
    arrays.append('#endif\n')
    (output / 'theme.h').write_text('\n'.join(arrays))

    # Render real C-planned patches into sample surfaces at RVM's logical size.
    canvas = [(*rgb(theme['palette']['desktop']), 255)] * (640 * 480)
    def skin(name, x, y, w, h, centre=True):
        draw(slice_fn, canvas, 640, 480, theme['assets'][name], sources[name], x, y, w, h, centre)
    skin('panel', 0, 0, 640, 24)
    skin('window-inactive', 330, 70, 280, 220)
    skin('window-active', 32, 48, 398, 346)
    skin('field', 48, 88, 366, 26)
    skin('field', 48, 126, 366, 214)
    skin('selection', 52, 154, 358, 23)
    skin('button-normal', 48, 352, 84, 26)
    skin('button-hover', 142, 352, 84, 26)
    skin('button-default', 320, 352, 94, 26)
    skin('panel', 164, 426, 312, 44)
    png(output / 'preview.png', 640, 480, canvas)
    study(canvas.copy(), theme, output)
    # Browser text is deliberately separate from asset painting: guest fonts
    # and icons need their own measured/licensed port, not host font assumptions.
    template = (HERE / 'tools/preview.html').read_text()
    for key, value in theme['palette'].items():
        template = template.replace('@@' + key + '@@', value)
    (output / 'preview.html').write_text(template)
    return meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--theme', type=Path, default=HERE / 'themes/slate.json')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fusede/slate')
    args = parser.parse_args()
    meta = build(json.loads(args.theme.read_text()), args.output.resolve())
    print(f"{meta['name']}: {meta['width']}x{meta['height']} atlas, {meta['pixel_octets']} RGB565 + {meta['mask_octets']} mask octets")
    print(args.output.resolve() / 'preview.html')


if __name__ == '__main__':
    main()
