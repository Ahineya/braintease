#!/usr/bin/env python3
"""Horizontal trackpad, recycled headers and resizing in a fresh offline guest."""
import hashlib,importlib.util,json,shlex
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];PORT=Path(__file__).resolve().parent;BUILD=ROOT/'build/lvgl'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
gui=module('gui',PORT/'gui-test.py');fixtures=module('fixtures',PORT/'data-fixtures.py')
def region(image,scale):
    w,h,p=image
    # Include header, frozen gutter and cells. Exclude overlay scrollbars/outline.
    return b''.join(p[(y*w+16*scale)*3:(y*w+784*scale)*3] for y in range(124*scale,486*scale))
def main():
    out=BUILD/'table-scroll-tests';out.mkdir(exist_ok=True);(out/'result.json').unlink(missing_ok=True)
    folder,files=fixtures.prepare();x=gui.GUI(out,{'lvdata':BUILD/'lvdata.fxe'},files);g=x.g
    def drag(wid,start,end):
        x.send(wid,input=dict(type='button',button=0,down=True,x=start,y=134))
        x.send(wid,input=dict(type='motion',x=end,y=134))
        x.send(wid,input=dict(type='button',button=0,down=False,x=end,y=134));x.pause()
    try:
        at=len(g.log);g.shell('lvdata --trace '+shlex.quote(str(folder/'wide.csv'))+' &');g.shell('viewer=$!')
        wid=x.wait(lambda:x.window('Data Viewer'))['id'];x.logged('DATA_OPEN ',at)
        # Actual pointer resize: first two columns 144 -> 320 logical points.
        x.send(wid,density=1);x.frame(wid);at=len(g.log)
        drag(wid,210,386);drag(wid,530,706)
        assert b'DATA_SORT ' not in g.log[at:],'Resize sorted a column'
        x.click(wid,110,166)
        for _ in range(4):x.key(wid,79)
        pixels={}
        for scale in (1,2):
            x.send(wid,density=scale);actual=x.frame(wid,scale=scale)
            reference=gui.ppm(BUILD/'host'/f'data-horizontal-{scale}x.ppm')
            x.snapshot(wid,f'horizontal-{scale}x',scale=scale)
            assert region(actual,scale)==region(reference,scale),f'Partial-column header/cell alignment at {scale}x'
            # Sub-line horizontal motion, crossing column boundaries and recycling.
            x.send(wid,input=dict(type='motion',x=450,y=300))
            for direction in (32768,-32768):
                for _ in range(15):x.send(wid,input=dict(type='wheel',x=direction,y=0))
            restored=x.frame(wid,scale=scale)
            assert region(restored,scale)==region(reference,scale),f'Horizontal wheel round trip at {scale}x'
            pixels[str(scale)]=len(region(restored,scale))//3
        # Selection remains in the same data column after header recycling.
        x.send(wid,density=1);x.frame(wid)
        at=len(g.log);x.click(wid,180,166);x.logged('DATA_SELECT 0 1',at)
        # Header at x=355 is the trailing edge of column 2 after sx=354.
        at=len(g.log);drag(wid,353,393)
        assert b'DATA_SORT ' not in g.log[at:],'Scrolled header resize sorted'
        at=len(g.log);x.click(wid,180,134);x.logged('DATA_SORT 1 0 first=R100C2',at)
        at=len(g.log);x.click(wid,180,134);x.logged('DATA_SORT 1 1 first=R9C2',at)
        # Header reuse far to the right, then back to the start.
        at=len(g.log);x.ctrl(wid,77);x.logged('DATA_SELECT 199 31',at)
        at=len(g.log);x.ctrl(wid,74);x.logged('DATA_SELECT 0 0',at)
        x.snapshot(wid,'returned-home')
        x.send(wid,close=True);x.wait(lambda:not x.window('Data Viewer'))
        assert 'STATUS:0' in g.shell('wait $viewer; echo STATUS:$?')
        settings=g.shell('cat /data/home/.lvdata')
        assert 'widths=320,360,144' in settings,'Scrolled resize did not update intended column'
        report=dict(status='passed',executable_sha256=hashlib.sha256((BUILD/'lvdata.fxe').read_bytes()).hexdigest(),pixels_equal=pixels,checks=['two actual column drags','partial-column header/gutter/cell pixels at 1x/2x','fractional horizontal wheel round trip across column boundaries at 1x/2x','click selection after scrolling','resize correct recycled header without sorting','sort correct recycled header in both directions','far-end and home header reuse','persistent scrolled-column width','close cleanup'])
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    finally:x.close()
if __name__=='__main__':main()
