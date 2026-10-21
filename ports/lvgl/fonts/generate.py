#!/usr/bin/env python3
"""Build portable LVGL A8 font tables from the pinned OFL Noto Sans source.

Requires Pillow and the pinned cached LVGL archive when regenerating; builds use the
checked-in tables. No runtime font service or rasterizer is required.
"""
from pathlib import Path
import hashlib
import json
import struct
import tarfile
import re
from PIL import ImageFont, __version__ as pillow_version, features

HERE=Path(__file__).resolve().parent
source=HERE/'NotoSans-Regular.ttf'
identity=json.loads((HERE/'source.json').read_text())
assert hashlib.sha256(source.read_bytes()).hexdigest()==identity['files'][source.name]['sha256']
raw=source.read_bytes()
u16=lambda at:struct.unpack_from('>H',raw,at)[0]
u32=lambda at:struct.unpack_from('>I',raw,at)[0]
tables={raw[at:at+4]:u32(at+8) for at in range(12,12+16*u16(4),16)}
cmap=tables[b'cmap'];supported=set()
for record in range(cmap+4,cmap+4+8*u16(cmap+2),8):
    sub=cmap+u32(record+4)
    if u16(sub)!=4:continue
    count=u16(sub+6)//2;ends=sub+14;starts=ends+2*count+2;deltas=starts+2*count;ranges=deltas+2*count
    for i in range(count):
        for cp in range(u16(starts+2*i),min(u16(ends+2*i),0xfffe)+1):
            delta=u16(deltas+2*i);span=u16(ranges+2*i)
            if not span:glyph=(cp+delta)&65535
            else:
                glyph=u16(ranges+2*i+span+2*(cp-u16(starts+2*i)))
                if glyph:glyph=(glyph+delta)&65535
            if glyph:supported.add(cp)
wanted=[*range(32,256),*range(0x370,0x530),*range(0x2000,0x2070),*range(0x2190,0x2300)]
letters=sorted(c for c in wanted if c in supported)
assert letters[:95]==list(range(32,127))
assert all(c in letters for c in range(0x400,0x500))
cyrillic_id=letters.index(0x400)+1
tail=letters[95:];tail_start=tail[0]
assert all(ord(c) in letters for c in 'Українські Ґґ Єє Іі Її café')
sizes=(12,14,16,24,28,32)
for size in sizes:
    font=ImageFont.truetype(str(source),size,layout_engine=ImageFont.Layout.BASIC)
    ascent,descent=font.getmetrics();bitmaps=[];descriptors=['    {0},'];unique={}
    for cp in letters:
        mask,offset=font.getmask2(chr(cp),mode='L',anchor='ls')
        w,h=mask.size;pixels=bytes(mask);key=(w,h,pixels)
        if key not in unique:
            unique[key]=len(bitmaps);bitmaps.extend(pixels)
        index=unique[key]
        advance=round(font.getlength(chr(cp))*16)
        descriptors.append(f'    {{.bitmap_index={index}UL,.adv_w={advance},.box_w={w},.box_h={h},.ofs_x={offset[0]},.ofs_y={-offset[1]-h}}},')
    assert len(bitmaps)<(1<<20)
    fallback=14 if size<=16 else 28
    lines=['/* SPDX-License-Identifier: OFL-1.1 */',
        '/* Generated from pinned Noto Sans. See LICENSE.NotoSans.txt and generate.py. */',
        '#include "lvgl.h"',
        f'extern const lv_font_t gx_symbols_{fallback};',
        f'static const uint8_t bitmap_{size}[]={{']
    lines += ['    '+','.join(map(str,bitmaps[i:i+32]))+',' for i in range(0,len(bitmaps),32)]
    lines += ['};',f'static const lv_font_fmt_txt_glyph_dsc_t glyphs_{size}[]={{',*descriptors,'};',
        f'static const uint16_t unicode_{size}[]={{',
        ','.join(str(c-tail_start) for c in tail),'};',
        f'static const lv_font_fmt_txt_cmap_t cmap_{size}[]={{',
        '{.range_start=32,.range_length=95,.glyph_id_start=1,.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},',
        f'{{.range_start=1024,.range_length=256,.glyph_id_start={cyrillic_id},.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}},',
        f'{{.range_start={tail_start},.range_length={letters[-1]-tail_start+1},.glyph_id_start=96,.unicode_list=unicode_{size},.list_length={len(tail)},.type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}}}};',
        f'static const lv_font_fmt_txt_dsc_t data_{size}={{.glyph_bitmap=bitmap_{size},.glyph_dsc=glyphs_{size},.cmaps=cmap_{size},.cmap_num=3,.bpp=8,.stride=1,.bitmap_format=0}};',
        f'const lv_font_t gx_font_{size}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={ascent+descent},.base_line={descent},.static_bitmap=1,.dsc=&data_{size},.fallback=&gx_symbols_{fallback},.underline_position=-2,.underline_thickness={max(1,size//16)}}};','']
    (HERE/f'font-{size}.c').write_text('\n'.join(lines))
    print(size,len(letters),len(bitmaps),ascent,descent)
(HERE/'generated.json').write_text(json.dumps({'pillow':pillow_version,'freetype':features.version_module('freetype2'),
    'codepoints':letters,'sizes':list(sizes),'source_sha256':identity['files'][source.name]['sha256']},indent=2)+'\n')

# The checkbox mark follows the validated static-A8 path on C16. Preserve the
# exact Font Awesome glyph from the pinned LVGL archive; unpack at build time.
root=HERE.parents[2]
archive=root/'build/gui-evaluation/lvgl-v9.6.0.tar.gz'
provenance=json.loads((HERE.parent/'PROVENANCE.json').read_text())['lvgl']
assert hashlib.sha256(archive.read_bytes()).hexdigest()==provenance['sha256']
icons=['/* SPDX-License-Identifier: OFL-1.1 */',
       '/* Font Awesome checkmark from pinned LVGL sources. See LICENSE.FontAwesome5.txt. */',
       '#include "lvgl.h"']
icon_sources={}
with tarfile.open(archive) as tar:
    for size in (14,28):
        name=f"lvgl-9.6.0/src/font/lv_font_montserrat_{size}.c"
        text=tar.extractfile(name).read().decode()
        icon_sources[str(size)]=dict(member=name,sha256=hashlib.sha256(text.encode()).hexdigest())
        def array(name):
            body=re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\};',text,re.S).group(1)
            return re.sub(r'/\*.*?\*/','',body,flags=re.S)
        bitmap=[int(v,0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+',array('glyph_bitmap'))]
        unicode=[int(v,0) for v in re.findall(r'0x[0-9a-fA-F]+|\d+',array('unicode_list_1'))]
        cmap=re.search(r'\.range_start = (\d+), \.range_length = \d+, \.glyph_id_start = (\d+),\s*\.unicode_list = unicode_list_1',text).groups()
        gid=int(cmap[1])+unicode.index(0xf00c-int(cmap[0]))
        glyphs=re.findall(r'\{([^{}]*)\}',array('glyph_dsc'))
        g={k:int(v) for k,v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)',glyphs[gid])}
        n=g['box_w']*g['box_h'];start=g['bitmap_index']
        pixels=[((bitmap[start+i//2]>>(4 if i%2==0 else 0))&15)*17 for i in range(n)]
        line_height=int(re.search(r'\.line_height = (\d+)',text).group(1))
        baseline=int(re.search(r'\.base_line = (\d+)',text).group(1))
        icons += [f'static const uint8_t check_{size}[]={{'+','.join(map(str,pixels))+'};',
            f'static const lv_font_fmt_txt_glyph_dsc_t desc_{size}[]={{'+'{0},'+
            '{.bitmap_index=0UL,.adv_w='+str(g['adv_w'])+',.box_w='+str(g['box_w'])+',.box_h='+str(g['box_h'])+',.ofs_x='+str(g['ofs_x'])+',.ofs_y='+str(g['ofs_y'])+'}};',
            f'static const lv_font_fmt_txt_cmap_t map_{size}[]={{'+
            '{.range_start=61452UL,.range_length=1,.glyph_id_start=1,.type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}};',
            f'static const lv_font_fmt_txt_dsc_t data_{size}={{.glyph_bitmap=check_{size},.glyph_dsc=desc_{size},.cmaps=map_{size},.cmap_num=1,.bpp=8,.stride=1}};',
            f'const lv_font_t gx_symbols_{size}={{.get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,.get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,.line_height={line_height},.base_line={baseline},.static_bitmap=1,.dsc=&data_{size},.fallback=&lv_font_montserrat_{size}}};']
(HERE/'symbols.c').write_text('\n'.join(icons)+'\n')
(HERE/'symbols.json').write_text(json.dumps({'archive_sha256':provenance['sha256'],'sources':icon_sources,
    'codepoints':[0xf00c],'sizes':[14,28],'format':'static A8; exact nibble expansion of original A4'},indent=2)+'\n')
