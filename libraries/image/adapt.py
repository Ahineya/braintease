#!/usr/bin/env python3
"""Reproduce the private, per-decoder C16 picojpeg adapter from pinned input."""
import hashlib,json,re
from pathlib import Path
HERE=Path(__file__).resolve().parent

def prepare():
    audit=json.loads((HERE/'vendor/source.json').read_text())
    for name,digest in audit['files'].items():
        if hashlib.sha256((HERE/'vendor'/name).read_bytes()).hexdigest()!=digest:raise ValueError('Changed picojpeg import: '+name)
    s=(HERE/'vendor/picojpeg.c').read_text()
    s=s.replace('#include "picojpeg.h"','#include "jpeg-codec.h"\n#include <stdint.h>')
    s=s.replace('typedef signed short    int16;','typedef int32_t         int16; /* Widen arithmetic before C16 promotions. */')
    # The Microwindows copy commented out SOS spectral reads but still
    # subtracted their lengths, leaving three header bytes in entropy data.
    s=s.replace('//uint8 spectral_start, spectral_end, successive_high, successive_low;', 'uint8 spectral_start, spectral_end, successive_high, successive_low;')
    for field in ['spectral_start','spectral_end','successive_high','successive_low']:
        s=s.replace('//'+field+' ',field+' ')
    s=s.replace('   left -= 3;\n\n   while (left)', '   if(spectral_start || spectral_end != 63 || successive_high || successive_low)return PJPG_UNSUPPORTED_MODE;\n   left -= 3;\n\n   while (left)')
    a=s.index('// 128 bytes\n');b=s.index('static void fillInBuf(void)',a)
    chunk=s[a:b]
    table=re.search(r'typedef struct HuffTableT.*?} HuffTable;',chunk,re.S).group()
    table=table.replace('uint16 mMinCode[16];','uint16 mCount;\n   uint16 mMinCode[16];')
    declarations=[];names=[]
    for line in chunk.splitlines():
        if re.match(r'^(?:static )?(?:uint8|uint16|int16|HuffTable|pjpeg_scan_type_t|pjpeg_need_bytes_callback_t|void)\s+\**g\w+',line):
            declarations.append(line.removeprefix('static '));names.extend(re.findall(r'\bg\w+',line))
    assert len(declarations)==49,len(declarations)
    context=table+'\n#define PJPG_MAX_IN_BUF_SIZE 256\ntypedef struct RiJpegState {\n'+'\n'.join(declarations)+'\nunsigned eof_padding;\n} RiJpegState;\n'
    tail=s[b:]
    for name in names:tail=re.sub(r'\b'+name+r'\b','state->'+name,tail)
    s=s[:a]+context+tail
    # Every helper receives its own decoder context, including forward declarations.
    functions=set(re.findall(r'^(?:static [^\n(]+?|unsigned char )(\w+)\(',s,re.M))
    assert 'decodeNextMCU' in functions and 'pjpeg_decode_init' in functions
    for name in sorted(functions,key=len,reverse=True):
        s=re.sub(r'\b'+name+r'\(\s*\)',name+'(state)',s)
        s=re.sub(r'\b'+name+r'\((?!state\))',name+'(state, ',s)
        s=re.sub(r'(^static [^\n(]*?\b'+name+r'\()state, void\)',r'\1RiJpegState *state)',s,flags=re.M)
        s=re.sub(r'(^static [^\n(]*?\b'+name+r'\()state, ',r'\1RiJpegState *state, ',s,flags=re.M)
        s=re.sub(r'(^unsigned char '+name+r'\()state, (?!void\))',r'static unsigned char '+name+'(RiJpegState *state, ',s,flags=re.M)
        s=re.sub(r'(^unsigned char '+name+r'\()state, void\)',r'static unsigned char '+name+'(RiJpegState *state)',s,flags=re.M)
    # No original external globals/symbols leak into applications.
    s=s.replace('   uint8 bytes = 0;','').replace('         bytes++;','')
    s=s.replace('const uint8 gWinogradQuant[]','static const uint8 gWinogradQuant[]')
    s=s.replace('stuffChar(state, (uint8)state->gBitBuf);','stuffChar(state, (uint8)(state->gBitBuf & 255));')
    s=s.replace('state->gInBufOfs--;','if (!state->gInBufOfs || state->gInBufLeft >= 255) {state->gCallbackStatus=PJPG_DECODE_ERROR;return;}\n   state->gInBufOfs--;')
    s=s.replace('state->gTemFlag = ~state->gTemFlag;','if (++state->eof_padding > 2) state->gCallbackStatus=PJPG_STREAM_READ_ERROR;\n         state->gTemFlag = ~state->gTemFlag;')
    start=s.index('static int16 getExtendOffset(');end=s.index('//------------------------------------------------------------------------------',start)
    s=s[:start]+'''static int16 getExtendOffset(RiJpegState *state, uint8 i)
{ return i <= 15 ? 1L - (1L << i) : 0; }
'''+s[end:]
    # Huffman symbols may never index beyond the actual table, including DC tables.
    s=s.replace('if (i == 16)\n         return 0;','if (i == 16) {state->gCallbackStatus=PJPG_DECODE_ERROR;return 0;}')
    s=s.replace('j = (uint8)(j + (code - pHuffTable->mMinCode[i]));','''if (code < pHuffTable->mMinCode[i] ||
       (uint16)j + code - pHuffTable->mMinCode[i] >= pHuffTable->mCount) {
      state->gCallbackStatus=PJPG_DECODE_ERROR;return 0;
   }
   j = (uint8)(j + (code - pHuffTable->mMinCode[i]));''')
    s=s.replace('huffCreate(state, bits, pHuffTable);','huffCreate(state, bits, pHuffTable);\n      pHuffTable->mCount=count;')
    for fn in ['readDHTMarker','readDQTMarker','readDRIMarker','skipVariableMarker']:
        s=s.replace('            '+fn+'(state);','            { uint8 status='+fn+'(state); if(status)return status; }')
    s=s.replace('uint16 r, dc;','uint16 r; int16 dc;')
    s=s.replace('dc = dc + state->gLastDC[componentID];','dc = dc + state->gLastDC[componentID];\n      if(dc < -2048 || dc > 2047)return PJPG_DECODE_ERROR;')
    s=s.replace('state->gCoeffBuf[0] = dc * pQ[0];','state->gCoeffBuf[0] = dc * pQ[0];\n      if(state->gCoeffBuf[0] < -32768L || state->gCoeffBuf[0] > 32767)return PJPG_DECODE_ERROR;')
    s=s.replace('state->gCoeffBuf[ZAG[k]] = ac * pQ[k];','state->gCoeffBuf[ZAG[k]] = ac * pQ[k];\n               if(state->gCoeffBuf[ZAG[k]] < -32768L || state->gCoeffBuf[ZAG[k]] > 32767)return PJPG_DECODE_ERROR;')
    # Unsigned 16-bit chroma products need a signed wide subtraction.
    for value in [44,227,179,91]:
        s=re.sub(r'- '+str(value)+r'U?;', '- '+str(value)+'L;',s)
    for fn,expr in [('clamp','s'),('addAndClamp','(int32_t)a+b'),('subAndClamp','(int32_t)a-b')]:
        start=s.index('static PJPG_INLINE uint8 '+fn+'(');end=s.index('\n}',start)+2
        signature=s[start:s.index('\n{',start)]
        s=s[:start]+signature+'\n{\n   int32_t value='+expr+';\n   return value<0?0:value>255?255:(uint8)value;\n}\n'+s[end:]
    # Serialize private header types only; decoder routines are private here.
    header=(HERE/'vendor/picojpeg.h').read_text()
    header=re.sub(r'^unsigned char pjpeg_decode_.*?;\n','',header,flags=re.M)
    prefix='/* Generated by libraries/image/adapt.py. Altered C16 version: explicit\n * octet extraction, per-decoder state, checked Huffman indices/markers,\n * and widened/bounded coefficient arithmetic. Original notice retained. */\n'
    for name,data in [('jpeg-codec.inc',prefix+s),('jpeg-codec.h',prefix+header)]:
        path=HERE/name
        if not path.exists() or path.read_text()!=data:path.write_text(data)
if __name__=='__main__':
    prepare()
    from adapt_stb import prepare as stb
    stb()
