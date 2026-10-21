#!/usr/bin/env python3
"""Extract Graphite's pinned Noto Sans A8 metrics into portable desktop assets.

Uses only checked-in tables and Python's standard library; no rasterizer or fetch.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
SOURCE=ROOT/'ports/lvgl/fonts'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def generate():
    identity=json.loads((SOURCE/'generated.json').read_text())
    points=identity['codepoints']
    assert points==sorted(set(points)) and all(ord(c) in points for c in 'CalculatorУкраїнські café…?')
    inputs={'generated.json':digest((SOURCE/'generated.json').read_bytes())}
    outputs={}
    for size in [12,24]:
        path=SOURCE/f'font-{size}.c';text=path.read_text();inputs[path.name]=digest(path.read_bytes())
        array=lambda name:re.search(r'\b'+name+r'\[\]\s*=\s*\{(.*?)\};',text,re.S).group(1)
        pixels=bytes(map(int,re.findall(r'\d+',array(f'bitmap_{size}'))))
        entries=re.findall(r'\{([^{}]*)\}',array(f'glyphs_{size}'))
        assert len(entries)==len(points)+1
        height,baseline=map(int,re.search(r'\.line_height=(\d+),\.base_line=(\d+)',text).groups())
        records=[]
        for cp,entry in zip(points,entries[1:]):
            g={key:int(value) for key,value in re.findall(r'\.(\w+)=(-?\d+)',entry)}
            assert g['bitmap_index']+g['box_w']*g['box_h']<=len(pixels)
            records.append(struct.pack('<IIHHHhh',cp,g['bitmap_index'],g['adv_w'],g['box_w'],g['box_h'],g['ofs_x'],g['ofs_y']))
        outputs[f'noto-{size}.bin']=struct.pack('<4sIHH',b'GDF1',len(points),height,baseline)+b''.join(records)+pixels
    outputs['LICENSE.NotoSans.txt']=(SOURCE/'LICENSE.NotoSans.txt').read_bytes()
    report={'format':'GDF1','description':'Pinned Graphite Noto Sans 12/24 A8 glyph tables; 12 logical pixels at 1x/2x; no runtime resampling or external font service.',
            'license':'OFL-1.1','source_ttf_sha256':identity['source_sha256'],'source_tables_sha256':inputs,
            'codepoint_count':len(points),'record_bytes':18,'assets':{name:{'sha256':digest(data),'bytes':len(data)} for name,data in outputs.items()}}
    outputs['provenance.json']=(json.dumps(report,indent=2)+'\n').encode()
    return outputs

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true');args=parser.parse_args()
    for name,data in generate().items():
        path=HERE/name
        if args.check:assert path.read_bytes()==data,f'Stale desktop font asset: {path}'
        else:path.write_bytes(data)
    print('Desktop Noto Sans tables verified' if args.check else 'Desktop Noto Sans tables generated')

if __name__=='__main__':main()
