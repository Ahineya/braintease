#!/usr/bin/env python3
"""Generate the bounded C16 drawing engine; never modify the pinned import."""
import argparse
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
ENGINE = 'devopen devdraw devclip devrgn devblit devfont'.split()


def replace(root, name, old, new, count=1):
    path = root / name
    text = path.read_text()
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'{name}: expected {count} occurrences, found {actual}: {old!r}')
    path.write_text(text.replace(old, new))


def adapt(destination):
    src = HERE / 'upstream/src'
    for directory in ['include', 'engine', 'drivers', 'fonts/compiled']:
        (destination / directory).mkdir(parents=True, exist_ok=True)
    for path in (src / 'include').glob('*.h'):
        shutil.copyfile(path, destination / 'include' / path.name)
    for name in [*(f'engine/{s}.c' for s in ENGINE), 'engine/devclip2.c',
                 'engine/devfont.h', 'drivers/genfont.c', 'drivers/fb.h',
                 'fonts/compiled/rom8x16.c']:
        shutil.copyfile(src / name, destination / name)
    # Generated ROM comments contain Latin-1 glyphs; RCC reads UTF-8 sources.
    font = destination / 'fonts/compiled/rom8x16.c'
    font.write_text(font.read_bytes().decode('latin1'))
    # sizeof counts C16 cells; bit widths are independent of address units.
    replace(destination, 'include/mwtypes.h', 'sizeof(MWCOORD)*8', 'sizeof(MWCOORD)*16')
    replace(destination, 'include/mwtypes.h', 'sizeof(MWIMAGEBITS) * 8', 'sizeof(MWIMAGEBITS) * 16')
    # A 640x480 framebuffer exceeds the 16-bit int range.
    replace(destination, 'include/device.h', 'unsigned int size;', 'uint32_t size;')
    replace(destination, 'include/mwtypes.h', 'int\t\tsize;\t\t/* size of screen memory*/',
            'uint32_t size; /* screen memory in C16 cells */')
    for name in ['devdraw', 'devfont']:
        replace(destination, f'engine/{name}.c', '((width + 15) >> 4) << 1',
                '((width + 15) >> 4) * sizeof(MWIMAGEBITS)')
    replace(destination, 'engine/devdraw.c', 'int oldm = gr_dashmask;', 'uint32_t oldm = gr_dashmask;')
    replace(destination, 'engine/devdraw.c', '(1 << bit)', '(1UL << bit)', 3)
    replace(destination, 'engine/devdraw.c', '(1<<bit)', '(1UL<<bit)')
    replace(destination, 'engine/devblit.c',
            '#if SWIEROS | !MW_FEATURE_IMAGES\n\tconvblit = psd->BlitCopyMaskMonoWordMSB;',
            '#if SWIEROS | !MW_FEATURE_IMAGES\n\tconvblit = data_format == MWIF_MONOWORDMSB ? psd->BlitCopyMaskMonoWordMSB : NULL;')
    # Start with the existing, verified 8x16 ROM font for all four font aliases.
    for symbol in ['font_winFreeSansSerif11x13', 'font_X6x13']:
        replace(destination, 'drivers/genfont.c', symbol, 'font_rom8x16', 3)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    destination = parser.parse_args().destination
    adapt(destination)
    from server_adapt import adapt_server
    adapt_server(destination)
    from apps_adapt import adapt_apps
    adapt_apps(destination)
