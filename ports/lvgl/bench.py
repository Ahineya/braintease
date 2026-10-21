#!/usr/bin/env python3
"""Measure the selector on a disposable offline disk without PPM capture I/O."""
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import argparse
ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
spec=importlib.util.spec_from_file_location('fixtures',PORT/'selector-fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--software-upload',action='store_true')
    parser.add_argument('--software-raster',action='store_true')
    parser.add_argument('--rgb565',action='store_true')
    parser.add_argument('--scale',choices=('auto','1','2'),default='auto')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    build=ROOT/'build/lvgl';folder,files=fixtures.prepare()
    results=[]
    with tempfile.TemporaryDirectory(prefix='lvgl-bench-') as directory:
        work=Path(directory)
        env=dict(os.environ)
        for key in ('FUSE_X_FRAMES','FUSE_X_FRAME','FUSE_X_TEST_EVENT'):os.environ.pop(key,None)
        try:g=Guest(work,'native',{'lvbench':build/'lvbench.fxe'},[dict(name='/dev/gfx',mode=0o20600,device=0x400),*files])
        finally:os.environ.clear();os.environ.update(env)
        try:
            for i in range(3):
                prefix='LVGL_PROFILE=1 '+('LVGL_SOFTWARE_UPLOAD=1 ' if args.software_upload else '')
                if args.scale!='auto':prefix+='LVGL_SCALE='+args.scale+' '
                if args.software_raster:prefix+='LVGL_SOFTWARE_RASTER=1 '
                if args.rgb565:prefix+='LVGL_RGB565=1 '
                text=g.shell(f'{prefix}lvbench {folder}',seconds=180)
                assert 'FILL_CHECK passed' in text,text
                assert 'FONT_CHECK passed' in text,text
                row={}
                for line in text.splitlines():
                    if line.startswith(('LVGL_BENCH ','LVGL_PROFILE ')):
                        row[line.split()[0]]=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',line))
                assert 'LVGL_BENCH' in row and 'LVGL_PROFILE' in row,text
                results.append(row);print(json.dumps(row),flush=True)
        finally:
            try:g.close()
            finally:(build/'bench-guest.log').write_bytes(g.log)
    (args.output or build/'bench.json').write_text(json.dumps(results,indent=2)+'\n')

if __name__=='__main__':main()
