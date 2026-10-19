#!/usr/bin/env python3
"""Build Ripple Web bitmap strikes; normal builds use the checked-in output.

Requires Pillow (FreeType) and the official liberation-fonts-ttf-2.1.5.tar.gz.
No TrueType parser, rasterizer, or floating-point font math runs in the guest.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import tarfile
import unicodedata
from PIL import ImageFont

HERE = Path(__file__).resolve().parent
SHA256 = '7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0'
URL = 'https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz'
SIZES = (16, 32)


def cmap(data):
    """Read the BMP format-4 coverage table, only for offline subsetting."""
    tables = struct.unpack_from('>H', data, 4)[0]
    for i in range(tables):
        tag, _, start, _ = struct.unpack_from('>4sIII', data, 12+i*16)
        if tag == b'cmap': break
    else: raise ValueError('missing cmap')
    for i in range(struct.unpack_from('>H', data, start+2)[0]):
        _, _, offset = struct.unpack_from('>HHI', data, start+4+i*8)
        at = start+offset
        if struct.unpack_from('>H', data, at)[0] == 4: break
    else: raise ValueError('missing BMP cmap')
    n = struct.unpack_from('>H', data, at+6)[0]//2
    ends = struct.unpack_from('>'+str(n)+'H', data, at+14)
    starts = struct.unpack_from('>'+str(n)+'H', data, at+16+2*n)
    return {cp for lo, hi in zip(starts, ends) for cp in range(lo, hi+1)}


def encode(values):
    result = bytearray()
    for value in values:
        value = (value*15+127)//255
        if result and result[-1]&15 == value and result[-1] < 240:
            result[-1] += 16
        else: result.append(value)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('archive', type=Path)
    a = p.parse_args()
    raw = a.archive.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256: raise ValueError('wrong font archive')
    codes = sorted(set([*range(32,127), *range(160,592), *range(768,1328),
                        *range(8192,8256), 8364, 0xfffd]))
    header = bytearray(b'RBFONT01'+struct.pack('<HHHHI',12,2,len(codes),0,0))
    header += struct.pack('<'+'H'*len(codes), *codes)
    records = bytearray(len(codes)*24*12)
    pixels = bytearray()
    hashes = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        for family, name in enumerate(('Sans','Serif','Mono')):
            for variant, style in enumerate(('Regular','Bold','Italic','BoldItalic')):
                file = f'Liberation{name}-{style}.ttf'
                data = archive.extractfile('liberation-fonts-ttf-2.1.5/'+file).read()
                hashes[file] = hashlib.sha256(data).hexdigest()
                coverage = cmap(data)
                for strike, size in enumerate(SIZES):
                    font = ImageFont.truetype(io.BytesIO(data),size,layout_engine=ImageFont.Layout.BASIC)
                    for i, cp in enumerate(codes):
                        at = (((family*4+variant)*2+strike)*len(codes)+i)*12
                        if cp not in coverage:
                            struct.pack_into('<I',records,at,0xffffffff)
                            continue
                        mask, (left, top) = font.getmask2(chr(cp),mode='L',anchor='ls')
                        w, h = mask.size
                        advance = int(font.getlength(chr(cp))+.5)
                        assert 0<=w<=64 and 0<=h<=64 and -64<=left<=64 and -64<=top<=64
                        packed = encode(mask)
                        struct.pack_into('<IHbbBBBB',records,at,len(header)+len(records)+len(pixels),
                                         len(packed),left,top,w,h,advance,0)
                        pixels += packed
        for file in ('LICENSE','AUTHORS'):
            (HERE/file).write_bytes(archive.extractfile('liberation-fonts-ttf-2.1.5/'+file).read())
    result = header+records+pixels
    struct.pack_into('<I',result,16,len(result))
    (HERE/'web.nfa').write_bytes(result)
    # Compose supported canonical pairs before rasterization. Uncomposed clusters
    # retain the shared Unicode bitmap fallback, with grapheme-safe byte offsets.
    pairs=[]
    for cp in codes:
        d=unicodedata.decomposition(chr(cp)).split()
        if len(d)==2 and not d[0].startswith('<'):
            pair=tuple(int(v,16) for v in d)
            if unicodedata.normalize('NFC',''.join(map(chr,pair)))==chr(cp):pairs.append((*pair,cp))
    (HERE/'compose.inc').write_text('/* Generated canonical composition pairs. */\n'
        'static const unsigned short nf_pairs[][3] = {\n'+''.join(
            '{%d,%d,%d},\n'%pair for pair in sorted(pairs))+'};\n')
    (HERE/'source.json').write_text(json.dumps(dict(name='Ripple Web bitmap subset',
        source=URL,sha256=SHA256,fonts=hashes,strikes=SIZES,codepoints=len(codes),
        pillow=__import__('PIL').__version__,freetype=__import__('PIL.features',fromlist=['version']).version('freetype2'),
        output_sha256=hashlib.sha256(result).hexdigest()),indent=2)+'\n')
    print(f'{len(result):,} font octets, {len(codes)} code points, 12 faces, 2 strikes')


if __name__=='__main__':main()
