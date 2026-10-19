#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline icon atlas builder: verifies pinned sources and consumes baked masks."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from build_theme import HERE, ROOT, png


def build(output):
    source = HERE / 'icons/lucide'
    provenance = json.loads((source / 'UPSTREAM.json').read_text())
    masks = json.loads((HERE / 'icons/masks.json').read_text())
    if masks['version'] != 1 or masks['revision'] != provenance['revision'] or masks['sizes'] != [16, 24, 32]:
        raise ValueError('unsupported icon raster version or source revision')
    names = set()
    for file in provenance['files']:
        if Path(file['file']).name != file['file']:
            raise ValueError('invalid source filename')
        if hashlib.sha256((source / file['file']).read_bytes()).hexdigest() != file['sha256']:
            raise ValueError('changed pinned icon source: ' + file['file'])
        if file['file'].endswith('.svg'): names.add(Path(file['file']).stem)
    if set(masks['icons']) != names:
        raise ValueError('baked masks do not match the pinned icon selection')
    width = 256
    words, assets = [], []
    y = 0
    for size in masks['sizes']:
        columns = width // (size + 2)
        rows = (len(names) + columns - 1) // columns
        words.extend([0] * (rows * (size + 2) * (width // 16)))
        for index, name in enumerate(sorted(names)):
            x, yy = (index % columns) * (size + 2) + 1, y + (index // columns) * (size + 2) + 1
            data = masks['icons'][name][str(size)]
            pitch = (size + 15) // 16
            if len(data) != pitch * size or any(type(w) is not int or not 0 <= w <= 65535 for w in data):
                raise ValueError(f'invalid baked mask: {name}/{size}')
            for sy in range(size):
                for sx in range(size):
                    if data[sy * pitch + sx // 16] & (0x8000 >> (sx % 16)):
                        words[(yy + sy) * (width // 16) + (x + sx) // 16] |= 0x8000 >> ((x + sx) % 16)
            assets.append(dict(name=name, size=size, x=x, y=yy))
        y += rows * (size + 2)
    if y > 512:
        raise ValueError('icon atlas exceeds image resource height')
    output.mkdir(parents=True, exist_ok=True)
    header = ['/* Lucide-derived raster masks: ISC / Feather MIT. See icons-LICENSE. */',
              '#ifndef FDE_ICON_DATA_H', '#define FDE_ICON_DATA_H', f'#define FDE_ICON_WIDTH {width}',
              f'#define FDE_ICON_HEIGHT {y}', 'struct fde_icon {unsigned x,y,size;};']
    for i, name in enumerate(sorted(names)):
        header.append(f'#define FDE_ICON_{name.upper().replace("-", "_")} {i}')
    header.append(f'#define FDE_ICON_COUNT {len(names)}')
    header.append(f'static const struct fde_icon fde_icons[{len(assets)}] = {{')
    header.extend(f'    {{{a["x"]},{a["y"]},{a["size"]}}},' for a in assets)
    header += ['};', f'static const unsigned short fde_icon_mask[{len(words)}] = {{']
    header.extend('    ' + ','.join(f'0x{w:04x}' for w in words[i:i + 12]) + ',' for i in range(0, len(words), 12))
    header += ['};', '#endif\n']
    (output / 'icons.h').write_text('\n'.join(header))
    (output / 'icons.mask').write_bytes(struct.pack('<' + 'H' * len(words), *words))
    (output / 'icons.fdi').write_bytes(b'FDI1' + struct.pack('<HHHH', width, y, 2, 0) +
                                      struct.pack('<' + 'H' * len(words), *words))
    (output / 'icons.json').write_text(json.dumps(dict(version=1, width=width, height=y, assets=assets,
                                                      revision=provenance['revision']), indent=2) + '\n')
    (output / 'icons-LICENSE').write_bytes((source / 'LICENSE').read_bytes())
    pixels = [(235, 238, 236, 255 if words[yy * (width // 16) + x // 16] & (0x8000 >> (x % 16)) else 0)
              for yy in range(y) for x in range(width)]
    png(output / 'icons.png', width, y, pixels)
    print(f'Lucide: {len(names)} icons at 16/24/32 px; {len(words)*2} mask octets')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fusede/slate')
    build(parser.parse_args().output.resolve())
