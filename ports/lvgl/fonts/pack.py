#!/usr/bin/env python3
"""Prepare lossless, two-octet C16 font storage without modifying baked assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Baked font layout changed: '+before)
    return text.replace(before, after)


def bitmap(source, size):
    pattern = rf'static const uint8_t bitmap_{size}\[\]=\{{(.*?)\}};'
    match = re.search(pattern, source, re.S)
    if not match:
        raise ValueError(f'Missing exact A8 bitmap declaration for {size}')
    body = match[1]
    if re.sub(r'[\d\s,]', '', body):
        raise ValueError('Unexpected token in baked A8 coverage')
    values = [int(v) for v in re.findall(r'\d+', body)]
    if not values or len(values) > 1048576 or any(v > 255 for v in values):
        raise ValueError('Invalid A8 bitmap length/coverage')
    return match, bytes(values)


def prepare(build):
    """Return the six C16-only adapted source paths, inside the given build."""
    build = Path(build).resolve()
    output = build/'packed-fonts'
    output.mkdir(parents=True, exist_ok=True)
    sizes = json.loads((HERE/'generated.json').read_text())['sizes']
    if sizes != [12, 14, 16, 24, 28, 32]:
        raise ValueError('Update font-data.c before changing the font set')
    fonts, manifest = [], []
    for size in sizes:
        original = HERE/f'font-{size}.c'
        source = original.read_text()
        match, coverage = bitmap(source, size)
        cells = [coverage[i] | ((coverage[i+1] if i+1 < len(coverage) else 0) << 8)
                 for i in range(0, len(coverage), 2)]
        # Always prove the transform locally, including an odd final octet.
        decoded = bytes((cells[i//2] >> ((i & 1)*8)) & 255 for i in range(len(coverage)))
        assert decoded == coverage
        packed = [f'static const uint16_t packed_bitmap_{size}[]={{']
        packed += ['    '+','.join(str(v) for v in cells[i:i+24])+',' for i in range(0, len(cells), 24)]
        packed += ['};']
        adapted = source[:match.start()]+'\n'.join(packed)+source[match.end():]
        adapted = replace_once(adapted, '#include "lvgl.h"', '#include "lvgl.h"\n#include "font-data.h"')
        adapted = replace_once(adapted, f'static const lv_font_fmt_txt_dsc_t data_{size}=',
                               f'static lv_font_fmt_txt_dsc_t data_{size}=')
        adapted = replace_once(adapted, f'.glyph_bitmap=bitmap_{size}', '.glyph_bitmap=NULL')
        adapted += (f'\n/* Generated C16 storage adapter; original glyph metrics/maps are unchanged. */\n'
                    f'int gx_font_data_{size}_open(void) {{\n'
                    f'    return gx_font_data_expand(&data_{size}, packed_bitmap_{size}, {len(coverage)}UL);\n}}\n'
                    f'void gx_font_data_{size}_close(void) {{ gx_font_data_release(&data_{size}); }}\n')
        target = output/f'font-{size}.c'
        if not target.exists() or target.read_text() != adapted:
            target.write_text(adapted)
        fonts.append(target)
        manifest.append(dict(size=size, source=str(original),
                             source_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
                             adapted_sha256=hashlib.sha256(adapted.encode()).hexdigest(),
                             coverage_sha256=hashlib.sha256(coverage).hexdigest(),
                             coverage_octets=len(coverage), packed_cells=len(cells),
                             saved_static_cells=len(coverage)-len(cells),
                             glyph_count=1+len(re.findall(r'\.bitmap_index=', source))))
    report = dict(format='two A8 octets per C16 uint16 static cell; original A8 restored once on heap',
                  fonts=manifest, coverage_octets=sum(f['coverage_octets'] for f in manifest),
                  packed_cells=sum(f['packed_cells'] for f in manifest),
                  saved_static_cells=sum(f['saved_static_cells'] for f in manifest))
    (build/'packed-font-data.json').write_text(json.dumps(report, indent=2)+'\n')
    return fonts


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    args = parser.parse_args()
    for path in prepare(args.build):
        print(path)
