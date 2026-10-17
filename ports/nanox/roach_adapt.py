#!/usr/bin/env python3
"""Adapt the pinned nxroach and convert its octet XBM assets to C16 masks."""
import re
import shutil
from pathlib import Path
from adapt import replace
from server_adapt import body

HERE = Path(__file__).resolve().parent


def adapt_roach(dest):
    source = HERE / 'upstream/src'
    for name in ('nxroach.c', 'nxroach.h'):
        shutil.copyfile(source / 'demos/nanox' / name, dest / 'demos/nanox' / name)
    # XBM pixels are packed LSB-first into octets. GR_BITMAP uses MSB-first
    # 16-bit cells, with each row separately padded to a complete cell.
    masks = ['/* Converted from the pinned nxroach XBM assets; see UPSTREAM.md. */']
    for angle in range(0, 360, 15):
        prefix = f'roach{angle:03}'
        text = (source / f'images/demos/nanox/nxroach/{prefix}.xbm').read_text()
        width = int(re.search(r'#define \w+_width\s+(\d+)', text)[1])
        height = int(re.search(r'#define \w+_height\s+(\d+)', text)[1])
        octets = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]+)', text)]
        stride = (width + 7) // 8
        if not 0 < width <= 48 or not 0 < height <= 48 or len(octets) != stride * height or any(v > 255 for v in octets):
            raise RuntimeError(f'{prefix}: invalid XBM dimensions/data')
        words = []
        for y in range(height):
            for x0 in range(0, width, 16):
                word = 0
                for bit in range(min(16, width - x0)):
                    x = x0 + bit
                    if octets[y * stride + x // 8] & (1 << (x % 8)):
                        word |= 0x8000 >> bit
                words.append(f'0x{word:04x}')
        masks += [f'#define {prefix}_width {width}', f'#define {prefix}_height {height}',
                  f'static GR_BITMAP {prefix}_bits[] = {{' + ','.join(words) + '};']
    (dest / 'demos/nanox/nxroach-bits.h').write_text('\n'.join(masks) + '\n')
    header = dest / 'demos/nanox/nxroach.h'
    text = header.read_text()
    text, count = re.subn(r'^#include .*$', '', text, flags=re.M)
    if count != 25:
        raise RuntimeError('nxroach.h: unexpected includes')
    text = '#include "nxroach-bits.h"\n' + text
    text = text.replace('char *roachBits;', 'GR_BITMAP *roachBits;')
    text = re.sub(r'(roach\d+)_height, (roach\d+)_width', r'\1_width, \2_height', text)
    header.write_text(text)
    name = 'demos/nanox/nxroach.c'
    replace(dest, name, '#include <time.h>', '#include <time.h>\n#include <errno.h>\n#include <unistd.h>')
    replace(dest, name, 'GR_BOOL done = GR_FALSE;', 'volatile GR_BOOL done = GR_FALSE;')
    replace(dest, name, 'MWPIXELVAL roachColor;', 'GR_COLOR roachColor = GR_RGB(0,0,0);')
    replace(dest, name, 'int MarkHiddenRoaches(void);', 'int MarkHiddenRoaches(void);\n#include "roach-platform.inc"')
    body(dest, name, 'main', '    return nxr_roach_main(ac, av);')
    body(dest, name, 'Usage', '    nxr_roach_usage();')
    body(dest, name, 'SigHandler', '    nxr_roach_signal(); /* Wake select without graphics or allocator calls. */')
    body(dest, name, 'DrawRoaches', '    nxr_roach_draw();')
    body(dest, name, 'CoverRoot', '    nxr_roach_clear();')
    body(dest, name, 'CalcRootVisible', '    return nxr_roach_visible();')
    replace(dest, name, 'r->intX > 0 &&', 'r->intX >= 0 &&')
