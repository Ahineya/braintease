#!/usr/bin/env python3
"""Real wide-window geometry, narrow-column coverage and Retina pixel checks."""
import hashlib,importlib.util,json,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
gui=module('gui',PORT/'gui-test.py');fixtures=module('fixtures',PORT/'data-fixtures.py')
def region(image,scale):
    w,h,p=image
    return b''.join(p[(y*w+16*scale)*3:(y*w+w-16*scale)*3] for y in range(124*scale,h-74*scale))
def main():
    out=BUILD/'wide-screen-tests';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    prefs=out/'preferences';prefs.write_text('header=1\ndelimiter=0\nwidths='+','.join(['64']*64)+'\n')
    folder,files=fixtures.prepare();files.append(dict(name='/data/home/.lvdata',path=str(prefs),mode=0o100600))
    x=gui.GUI(out,{'lvdata':BUILD/'lvdata.fxe'},files);g=x.g
    def rendered(reference,scale,partial=False):
        # The model trace precedes rendering. A large cold repaint can take
        # longer than GUI.frame's generic pause; wait for the published pixels.
        def ready():
            try:actual=gui.ppm(x.frames/f'{wid}.ppm')
            except OSError:return None
            if not actual or actual[:2]!=reference[:2]:return None
            equal=region(actual,scale)==region(reference,scale) if partial else actual==reference
            return actual if equal else None
        return x.wait(ready)
    try:
        at=len(g.log);g.shell('lvdata --trace '+shlex.quote(str(folder/'wide.csv'))+' &');g.shell('viewer=$!')
        wid=x.wait(lambda:x.window('Data Viewer'))['id'];x.logged('DATA_OPEN ',at);pixels={}
        for width,height in ((1470,900),(1920,1080)):
            x.send(wid,density=1);x.send(wid,resize=[width,height]);x.ctrl(wid,74)
            for scale in (1,2):
                x.send(wid,density=scale)
                actual=x.frame(wid,width=width,height=height,scale=scale)
                reference=gui.ppm(BUILD/'host'/f'data-wide-screen-{width}x{height}-{scale}x.ppm')
                actual=rendered(reference,scale)
                x.snapshot(wid,f'wide-{width}x{height}-{scale}x',width=width,height=height,scale=scale)
                # Hit testing and rendering must extend beyond the old 1024px cap.
                col=(width-80-12-56)//64
                at=len(g.log);x.click(wid,width-80,166);x.logged(f'DATA_SELECT 0 {col}',at)
                at=len(g.log);x.ctrl(wid,77);x.logged('DATA_SELECT 199 31',at)
                actual=x.frame(wid,width=width,height=height,scale=scale)
                x.snapshot(wid,f'end-{width}x{height}-{scale}x',width=width,height=height,scale=scale)
                end=gui.ppm(BUILD/'host'/f'data-wide-screen-{width}x{height}-{scale}x-end.ppm')
                actual=rendered(end,scale,partial=True)
                at=len(g.log);x.ctrl(wid,74);x.logged('DATA_SELECT 0 0',at)
                x.send(wid,input=dict(type='motion',x=450,y=300))
                for axis in ('x','y'):
                    for direction in (-32768,32768):
                        for _ in range(3):x.send(wid,input=dict(type='wheel',x=direction if axis=='x' else 0,y=direction if axis=='y' else 0))
                actual=x.frame(wid,width=width,height=height,scale=scale)
                actual=rendered(reference,scale,partial=True)
                pixels[f'{width}x{height}@{scale}']=len(actual[2])//3
        # Requests beyond the hosted device's bounds stay within its supported surface.
        x.send(wid,resize=[2100,1200]);x.frame(wid,width=1920,height=1080,scale=2)
        x.send(wid,density=1);x.send(wid,resize=[800,560]);x.frame(wid)
        x.send(wid,close=True);x.wait(lambda:not x.window('Data Viewer'))
        assert 'STATUS:0' in g.shell('wait $viewer; echo STATUS:$?')
        report=dict(status='passed',executable_sha256=hashlib.sha256((BUILD/'lvdata.fxe').read_bytes()).hexdigest(),pixels_equal=pixels,checks=['1470x900 and 1920x1080 logical sizes at 1x/2x','exact host/guest pixels across the full client area','64-point columns exceed previous 18-column pool','pointer hit testing beyond 1024px','last column/record and home restoration','two-axis wheel cache reuse','hosted device bounds','resize back to windowed','close cleanup'])
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    finally:x.close()
if __name__=='__main__':main()
