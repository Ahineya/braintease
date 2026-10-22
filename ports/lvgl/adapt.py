#!/usr/bin/env python3
"""Generate the narrow C16 LVGL port without changing the pristine release."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tarfile

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('LVGL adaptation contract changed: ' + old)
    return text.replace(old, new)


def prepare(build, fetch=False):
    spec = importlib.util.spec_from_file_location('lvgl_probe', PORT / 'probe.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    cache = ROOT / 'build/gui-evaluation'
    cache.mkdir(parents=True, exist_ok=True)
    pristine, _, _ = probe.prepare(cache, fetch)
    source = build / 'src'
    shutil.copytree(pristine, source, dirs_exist_ok=True)
    # C16 encodes source string characters as UTF-8, including hex escapes.
    # Upstream writes already-encoded UTF-8 octets as escapes. Decode these
    # to source characters once, rather than encoding each octet a second time.
    symbol_header = source / 'include/lvgl/font/lv_symbol_def.h'
    symbols, count = re.subn(r'"((?:\\x[0-9a-fA-F]{2})+)"',
        lambda m: '"' + bytes(int(v, 16) for v in re.findall(r'\\x([0-9a-fA-F]{2})', m[1])).decode('utf-8') + '"',
        symbol_header.read_text())
    assert count > 50, 'LVGL symbol encoding contract changed'
    symbol_header.write_text(symbols)
    config = build / 'config'
    config.mkdir(parents=True, exist_ok=True)
    text = (source / 'lv_conf_template.h').read_text()
    text = replace(text, '#if 0 /* Set this to "1" to enable content */', '#if 1 /* C16 port */')
    keep = {'LV_USE_DRAW_SW', 'LV_USE_THEME_SIMPLE', 'LV_USE_FLEX', 'LV_USE_GRID',
            'LV_USE_LABEL', 'LV_USE_BUTTON', 'LV_USE_TEXTAREA', 'LV_USE_CHECKBOX',
            'LV_USE_FONT_PLACEHOLDER'}
    text = re.sub(r'(#define\s+)(LV_USE_\w+)(\s+)1\b',
                  lambda m: m[1] + m[2] + m[3] + str(int(m[2] in keep)), text)
    for name in ('MALLOC', 'STRING', 'SPRINTF'):
        text = re.sub(r'(#define\s+LV_USE_STDLIB_' + name + r'\s+)LV_STDLIB_BUILTIN',
                      r'\g<1>LV_STDLIB_CLIB', text)
    text = re.sub(r'(#define\s+LV_DRAW_SW_SUPPORT_(?!RGB888\s|ARGB8888\s|A8\s)\w+\s+)1\b', r'\g<1>0', text)
    for name, value in {'LV_FONT_MONTSERRAT_14':'1', 'LV_FONT_MONTSERRAT_20':'0', 'LV_FONT_MONTSERRAT_28':'1',
                        'LV_FONT_DEFAULT':'LV_FONT_DEFAULT_MONTSERRAT_14',
                        'LV_USE_LOG':'1', 'LV_LOG_PRINTF':'1',
                        'LV_USE_ASSERT_NULL':'1', 'LV_USE_ASSERT_MALLOC':'1'}.items():
        text, count = re.subn(r'(^#define\s+' + name + r'\s+)[^\n]+', r'\g<1>' + value, text, flags=re.M)
        assert count == 1, name
    text = replace(text, '#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565',
                   '#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB888')
    # Assertions must terminate the owning application, never spin forever in
    # this cooperative guest. Use the same policy in the host reference.
    text = text.replace('#define LV_USE_ASSERT_MALLOC 1',
                        '#include <stdlib.h>\n#include <limits.h>\n#define LV_ASSERT_HANDLER abort();\n#define LV_USE_ASSERT_MALLOC 1')
    (config / 'lv_conf.h').write_text(text)
    (config / 'inttypes.h').write_text('#include <stdint.h>\n' + ''.join(
        '#define PRI' + kind + str(bits) + ' "' + suffix + kind + '"\n'
        for bits, suffix in ((32, 'l'), (64, 'll')) for kind in ('d', 'u', 'x', 'X')))

    # RGB888 has one logical octet per C cell on both architectures. Retain
    # upstream scalar fallbacks instead of packing four octets into uint32_t.
    # Submit primary-display solid and A8 blends before LVGL turns coordinates
    # into C16 RGB buffer pointers. The adapter owns queued mask copies and
    # restores that buffer before an unsupported operation falls back to LVGL.
    # Offscreen layers and the byte-addressed reference keep upstream drawing.
    path = source / 'src/draw/sw/blend/lv_draw_sw_blend.c'
    text = replace(path.read_text(), '#if LV_USE_DRAW_SW', '''#if LV_USE_DRAW_SW
#if CHAR_BIT == 16
extern int gx_lv_render_blend(lv_draw_task_t *, const lv_draw_sw_blend_dsc_t *);
#endif''')
    text = replace(text, '''    LV_PROFILER_DRAW_BEGIN;
    lv_layer_t * layer = t->target_layer;''', '''#if CHAR_BIT == 16
    if(gx_lv_render_blend(t, dsc)) return;
#endif
    LV_PROFILER_DRAW_BEGIN;
    lv_layer_t * layer = t->target_layer;''')
    path.write_text(text)
    # Desktop panels retain straight alpha. A C16 lv_color32_t has four
    # logical channel cells, whereas uint32_t occupies two: upstream's packed
    # fill and four-octet mask loads cannot alias this representation. Use the
    # same scalar structure/channel path as other byte-addressed targets.
    path = source / 'src/draw/sw/blend/lv_draw_sw_blend_to_argb8888.c'
    text = replace(path.read_text(),
                   'uint32_t color32 = lv_color_to_u32(dsc->color);\n            uint32_t * dest_buf = dsc->dest_buf;',
                   'lv_color32_t color32 = lv_color_to_32(dsc->color, 255);\n            lv_color32_t * dest_buf = dsc->dest_buf;')
    start = text.index('            lv_color32_t color32 = lv_color_to_32(dsc->color, 255);')
    end = text.index('\n        }\n    }\n    /*Opacity only*/', start)
    scalar = text[start:end]
    # Opaque panel fills use the same bank-aware bulk-copy strategy as RGB.
    # Keep upstream's byte-addressed reference and all alpha mixing unchanged.
    text = text[:start] + '''#if CHAR_BIT == 16
            if(w > 0 && h > 0) {
                lv_color32_t color = lv_color_to_32(dsc->color, 255);
                uint8_t * first = dsc->dest_buf;
                uint32_t row_cells = (uint32_t)w * 4UL, filled = 4;
                lv_memcpy(first, &color, sizeof(color));
                while(filled < row_cells) {
                    uint32_t n = LV_MIN(filled, row_cells - filled);
                    lv_memcpy(first + filled, first, n);
                    filled += n;
                }
                for(y = 1; y < h; y++)
                    lv_memcpy(first + (uint32_t)y * dest_stride, first, row_cells);
            }
#else
''' + scalar + '\n#endif' + text[end:]
    text = replace(text, 'if(w < 32) {', 'if(w < 32 || CHAR_BIT != 8) {')
    text = replace(text, 'color_argb.alpha = (opa * mask_buf[dest_x]) >> 8;',
                   'color_argb.alpha = ((uint32_t)opa * mask_buf[dest_x]) >> 8;')
    # Non-normal blends are uncommon here, but retain correct intermediate
    # widths instead of relying on the host's 32-bit int promotions.
    for channel in ('red', 'green', 'blue'):
        text = text.replace('(dest->' + channel + ' * src.' + channel + ')',
                            '((uint32_t)dest->' + channel + ' * src.' + channel + ')')
        text = text.replace('dest->' + channel + ' - src.' + channel,
                            '(int32_t)dest->' + channel + ' - src.' + channel)
    path.write_text(text)
    path = source / 'src/draw/sw/blend/lv_draw_sw_blend_to_rgb888.c'
    text = path.read_text()
    text=replace(text,'#include "lv_draw_sw_blend_private.h"', '''#include "lv_draw_sw_blend_private.h"
#if CHAR_BIT == 16
extern int gx_lv_blend(void *,long,long,long,const void *,long,unsigned,unsigned,unsigned,unsigned);
#endif''')
    text=replace(text,'''void LV_ATTRIBUTE_FAST_MEM lv_draw_sw_blend_color_to_rgb888(lv_draw_sw_blend_fill_dsc_t * dsc, uint32_t dest_px_size)
{''','''void LV_ATTRIBUTE_FAST_MEM lv_draw_sw_blend_color_to_rgb888(lv_draw_sw_blend_fill_dsc_t * dsc, uint32_t dest_px_size)
{
#if CHAR_BIT == 16
    if(dest_px_size == 3 && gx_lv_blend(dsc->dest_buf,dsc->dest_w,dsc->dest_h,dsc->dest_stride,
        dsc->mask_buf,dsc->mask_stride,dsc->color.blue,dsc->color.green,dsc->color.red,dsc->opa)) return;
#endif''')
    assert text.count('#if LV_BIG_ENDIAN_SYSTEM == 0') == 5
    text = text.replace('#if LV_BIG_ENDIAN_SYSTEM == 0', '#if LV_BIG_ENDIAN_SYSTEM == 0 && CHAR_BIT == 8')
    assert text.count('uint32_t color32 = lv_color_to_u32(dsc->color);\n            uint8_t * dest_buf') == 2
    text = text.replace('uint32_t color32 = lv_color_to_u32(dsc->color);\n            uint8_t * dest_buf',
                        'lv_color_t color32 = dsc->color;\n            uint8_t * dest_buf')
    assert text.count('(uint8_t)(rb >> 8)') == 2
    text = text.replace('(uint8_t)(rb >> 8)', '(uint8_t)((rb >> 8) & 0xffU)')
    # C16 scalar stores cross the checked guest-memory boundary. Seed one
    # BGR pixel and double the initialized prefix with non-overlapping bulk
    # copies, then copy that row. Keep padding and neighbouring pixels intact.
    text = replace(text, '''                for(y = 0; y < h; y++) {
                    for(x = 0; x < w; x += 3) {
                        dest_buf_u8[x + 0] = dsc->color.blue;
                        dest_buf_u8[x + 1] = dsc->color.green;
                        dest_buf_u8[x + 2] = dsc->color.red;
                    }
                    dest_buf_u8 += dest_stride;
                }''', '''                if(w > 0 && h > 0) {
                    uint8_t * first = dest_buf_u8;
                    uint32_t filled = 3;
                    first[0] = dsc->color.blue;
                    first[1] = dsc->color.green;
                    first[2] = dsc->color.red;
                    while(filled < (uint32_t)w) {
                        uint32_t n = LV_MIN(filled, (uint32_t)w - filled);
                        lv_memcpy(first + filled, first, n);
                        filled += n;
                    }
                    for(y = 1; y < h; y++) {
                        dest_buf_u8 += dest_stride;
                        lv_memcpy(dest_buf_u8, first, w);
                    }
                }''')
    path.write_text(text)
    path = source / 'include/lvgl/misc/lv_math.h'
    text = replace(path.read_text(), '#define LV_UDIV255(x) (((x) * 0x8081U) >> 0x17)',
                   '#define LV_UDIV255(x) (((x) * 0x8081UL) >> 0x17)')
    path.write_text(text)
    # RCC's enum object representation is int16. Preserve LVGL's wide flag,
    # part and event identifiers with explicit storage types; constants stay
    # in their enum declarations, so symbolic source APIs are unchanged.
    for filename, definitions in {
        'include/lvgl/core/lv_obj.h': [('lv_obj_flag_t','uint32_t')],
        'include/lvgl/core/lv_obj_style.h': [('lv_state_t','uint16_t'),('lv_part_t','uint32_t')],
        'include/lvgl/core/lv_event.h': [('lv_event_code_t','uint32_t')],
    }.items():
        path=source/filename; text=path.read_text()
        for name, storage in definitions:
            end=text.index('} '+name+';');start=text.rfind('typedef enum {',0,end)
            assert start>=0
            text=text[:start]+'enum {'+text[start+len('typedef enum {'):end]+'};\ntypedef '+storage+' '+name+';'+text[end+len('} '+name+';'):]
        path.write_text(text)
    # Image headers have packed uint32_t bitfields. A C16 character read can
    # contain both the magic and format fields; classify only the low octet.
    path = source / 'src/draw/lv_draw_image.c'
    text = replace(path.read_text(), 'const uint8_t first_byte = *(const uint8_t *)src;',
                   'const uint8_t first_byte = *(const uint8_t *)src & 0xffU;')
    path.write_text(text)
    # LVGL assumes at least 32-bit integer literals in coordinate tags, flags,
    # masks, and fixed-point constants. C16's plain int has only sixteen bits.
    # Long is 32-bit in the guest; existing long/ULL literals remain unchanged.
    shifts = {}
    for path in [*sources(source), *sorted((source / 'include').rglob('*.h'))]:
        text, count = re.subn(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)([uU]?)(\s*<<)',
                             lambda m: m[1] + m[2] + 'L' + m[3], path.read_text())
        if count:
            path.write_text(text); shifts[str(path.relative_to(source))] = count
    path = source / 'src/misc/lv_style.c'
    text = path.read_text()
    assert text.count('i = style->prop_cnt - 1; i >= 0; i--') == 2
    text = text.replace('i = style->prop_cnt - 1; i >= 0; i--',
                        'i = (int32_t)style->prop_cnt - 1; i >= 0; i--')
    # RCC currently stores the initializer expression width for union
    # compound literals. Spell out the width of the numeric union member.
    text, count = re.subn(r'(\.num = )([^\n]+)', r'\g<1>(int32_t)(\2)', text)
    assert count == 8
    path.write_text(text)
    path = source / 'include/lvgl/layouts/lv_layout.h'
    text = replace(path.read_text(), 'typedef struct {\n    lv_layout_update_cb_t layout_update_cb;',
                   'typedef struct lv_layout_callbacks {\n    lv_layout_update_cb_t layout_update_cb;')
    path.write_text(text)
    path = source / 'src/core/lv_refr.c'
    text = replace(path.read_text(), 'lv_area_t res[4] = {0};',
                   'lv_area_t res[4]; lv_memzero(res, sizeof(res));')
    path.write_text(text)
    path = source / 'src/misc/lv_color.c'
    text = path.read_text()
    text = replace(text, '(color.red << 16)', '((uint32_t)color.red << 16)')
    text = replace(text, '(tmp[2] << 16)', '((uint32_t)tmp[2] << 16)')
    text = replace(text, 'return *((uint32_t *)&c1) == *((uint32_t *)&c2);',
                   'return c1.red == c2.red && c1.green == c2.green && c1.blue == c2.blue && c1.alpha == c2.alpha;')
    path.write_text(text)
    changes = {}
    for path in [*source.rglob('*.c'), *source.rglob('*.h')]:
        relative=path.relative_to(source)
        before=(pristine/relative).read_bytes();after=path.read_bytes()
        if before!=after:
            changes[str(relative)]={'before':hashlib.sha256(before).hexdigest(),'after':hashlib.sha256(after).hexdigest()}
    manifest = {'upstream': probe.SHA256, 'profile': 'RGB888/ARGB8888/A8 logical octets; no packed 8-bit word stores',
                'wide_literal_shifts': shifts, 'changes': changes}
    (build / 'adaptation.json').write_text(json.dumps(manifest, indent=2) + '\n')
    licenses = build / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copy2(PORT/'fonts/LICENSE.NotoSans.txt',licenses/'LICENSE.NotoSans.txt')
    shutil.copy2(PORT/'icons/LICENSE.Lucide.txt',licenses/'LICENSE.Lucide.txt')
    shutil.copy2(source / 'LICENCE.txt', licenses / 'LICENSE.LVGL.txt')
    with tarfile.open(cache / ('lvgl-v' + probe.VERSION + '.tar.gz')) as archive:
        for font, filename in [('Montserrat','OFL.txt'), ('FontAwesome5','LICENSE.txt')]:
            member = 'lvgl-' + probe.VERSION + '/scripts/generators/built_in_font/font_license/' + font + '/' + filename
            (licenses / ('LICENSE.' + font + '.txt')).write_bytes(archive.extractfile(member).read())
    return source, config


def sources(source):
    result = []
    for path in sorted((source / 'src').rglob('*.c')):
        parts = path.relative_to(source / 'src').parts
        if parts[0] in ('libs', 'drivers', 'others'): continue
        if parts[0] == 'draw' and len(parts) > 2 and parts[1] not in ('sw', 'convert'): continue
        if parts[0] == 'widgets' and parts[1] not in ('button', 'label', 'textarea', 'checkbox'): continue
        result.append(path)
    return result
