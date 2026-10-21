#!/usr/bin/env python3
"""Profile table scrolling in a fresh offline guest, without frame capture."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
import sys
import tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
BUILD=ROOT/'build/lvgl'
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
spec=importlib.util.spec_from_file_location('fixtures',PORT/'data-fixtures.py')
fixtures=importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--size',choices=('800x560','1024x1024','1470x900','1920x1080'),default='800x560')
    p.add_argument('--events-per-frame',type=int,default=1)
    p.add_argument('--column-width',type=int,default=320)
    p.add_argument('--axis',choices=('vertical','horizontal'),default='vertical')
    p.add_argument('--binary',type=Path,default=BUILD/'lvtablebench.fxe')
    p.add_argument('--baseline',type=Path,help='Alternate fresh before/after processes within the same guest')
    p.add_argument('--runs',type=int,default=3)
    p.add_argument('--scale',choices=('1','2','both'),default='both')
    p.add_argument('--rows',choices=('small','large','wide','both'),default='both')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    if args.runs<1 or not 64<=args.column_width<=480 or not 1<=args.events_per_frame<=40:p.error('Use positive runs, a column width from 64 to 480, and 1 to 40 events per frame')
    width,height=map(int,args.size.split('x'))
    folder,files=fixtures.prepare()
    cases=['sample.csv','large.csv'] if args.rows=='both' else [{'large':'large.csv','wide':'wide.csv','small':'sample.csv'}[args.rows]]
    scales=[1,2] if args.scale=='both' else [int(args.scale)]
    apps={'lvtablebench':args.binary}
    variants=[('after','lvtablebench')]
    if args.baseline:
        apps['lvtablebefore']=args.baseline
        variants.insert(0,('before','lvtablebefore'))
    results=[]
    with tempfile.TemporaryDirectory(prefix='lvtable-bench-') as directory:
        env=dict(os.environ)
        for name in ('FUSE_X_FRAME','FUSE_X_FRAMES','FUSE_X_TEST_EVENT'):os.environ.pop(name,None)
        try:g=Guest(Path(directory),'native',apps,[dict(name='/dev/gfx',mode=0o20600,device=0x400),*files],blocks=65536)
        finally:os.environ.clear();os.environ.update(env)
        try:
            for case in cases:
                for scale in scales:
                    for run in range(args.runs):
                        for variant,command in (list(reversed(variants)) if run%2 else variants):
                            text=g.shell('HOME=/tmp TABLE_BENCH_SCREEN_WIDTH='+str(width)+' TABLE_BENCH_SCREEN_HEIGHT='+str(height)+' TABLE_BENCH_EVENTS='+str(args.events_per_frame)+' TABLE_BENCH_WIDTH='+str(args.column_width)+' LVGL_SCALE='+str(scale)+' LVGL_PROFILE=1 '+command+' '+shlex.quote(str(folder/case))+(' horizontal' if args.axis=='horizontal' else ''),seconds=180)
                            row=dict(size=args.size,file=case,scale=scale,run=run,variant=variant,axis=args.axis,events_per_frame=args.events_per_frame,column_width=args.column_width if args.axis=='horizontal' else None)
                            for line in text.splitlines():
                                if line.startswith(('TABLE_BENCH ','LVGL_PROFILE ')):
                                    row[line.split()[0]]=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',line))
                            assert 'TABLE_BENCH' in row,text
                            results.append(row)
                            print(json.dumps(row),flush=True)
        finally:
            try:g.close()
            finally:args.output.with_suffix('.log').write_bytes(g.log)
    report=dict(executable_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest(),results=results)
    if args.baseline:report['baseline_sha256']=hashlib.sha256(args.baseline.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
