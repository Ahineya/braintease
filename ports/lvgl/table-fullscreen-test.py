#!/usr/bin/env python3
"""Fullscreen two-axis table scrolling, rendered-cache reuse and density changes."""
import hashlib,importlib.util,json,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
gui=module('gui',PORT/'gui-test.py');fixtures=module('fixtures',PORT/'data-fixtures.py')
def region(image,scale):
    w,h,p=image
    return b''.join(p[(y*w+16*scale)*3:(y*w+1008*scale)*3] for y in range(124*scale,950*scale))
def main():
    out=BUILD/'table-fullscreen-tests';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    folder,files=fixtures.prepare();x=gui.GUI(out,{'lvdata':BUILD/'lvdata.fxe'},files);g=x.g
    try:
        at=len(g.log);g.shell('lvdata --trace '+shlex.quote(str(folder/'wide.csv'))+' &');g.shell('viewer=$!')
        wid=x.wait(lambda:x.window('Data Viewer'))['id'];x.logged('DATA_OPEN ',at)
        x.send(wid,resize=[1024,1024]);pixels={}
        for scale in (1,2):
            x.send(wid,density=scale);initial=x.frame(wid,width=1024,height=1024,scale=scale)
            reference=gui.ppm(BUILD/'host'/f'data-fullscreen-{scale}x.ppm')
            assert initial==reference,f'Fullscreen initial pixels at {scale}x'
            x.snapshot(wid,f'fullscreen-{scale}x',width=1024,height=1024,scale=scale)
            x.send(wid,input=dict(type='motion',x=450,y=300))
            # Exceed the 39-row cache ring while keeping the selected cell fixed.
            for direction in (-65536,65536):
                for _ in range(20):x.send(wid,input=dict(type='wheel',x=0,y=direction))
            actual=x.frame(wid,width=1024,height=1024,scale=scale)
            assert region(actual,scale)==region(reference,scale),f'Fullscreen vertical cache reuse at {scale}x'
            for direction in (-32768,32768):
                for _ in range(20):x.send(wid,input=dict(type='wheel',x=direction,y=0))
            actual=x.frame(wid,width=1024,height=1024,scale=scale)
            assert region(actual,scale)==region(reference,scale),f'Fullscreen horizontal cache reuse at {scale}x'
            at=len(g.log);x.ctrl(wid,77);x.logged('DATA_SELECT 199 31',at)
            actual=x.frame(wid,width=1024,height=1024,scale=scale)
            end=gui.ppm(BUILD/'host'/f'data-fullscreen-end-{scale}x.ppm')
            assert region(actual,scale)==region(end,scale),f'Fullscreen last row/column at {scale}x'
            at=len(g.log);x.ctrl(wid,74);x.logged('DATA_SELECT 0 0',at)
            actual=x.frame(wid,width=1024,height=1024,scale=scale)
            assert region(actual,scale)==region(reference,scale),f'Fullscreen restored viewport at {scale}x'
            pixels[str(scale)]=len(region(actual,scale))//3
        x.send(wid,density=1);x.send(wid,resize=[800,560]);x.frame(wid)
        x.send(wid,close=True);x.wait(lambda:not x.window('Data Viewer'))
        assert 'STATUS:0' in g.shell('wait $viewer; echo STATUS:$?')
        report=dict(status='passed',executable_sha256=hashlib.sha256((BUILD/'lvdata.fxe').read_bytes()).hexdigest(),pixels_equal=pixels,checks=['1024x1024 logical viewport at 1x/2x','exact initial host/guest pixels','vertical wheel recycling beyond cache ring','horizontal fractional wheel recycling','exact last row and column pixels','return-to-home cache contents','live density and window-size transitions','close cleanup'])
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    finally:x.close()
if __name__=='__main__':main()
