#!/usr/bin/env python3
"""Actual guest font rendering, alpha over dark/light backgrounds and resize."""
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/fusede/tools'))
from build_theme import png

PAGE='''<!doctype html><meta charset="utf-8"><title>Readable fonts</title><style>
html,body{margin:0;padding:0;background:#fff;color:#17212b;font-family:sans-serif}
.sample{position:absolute;left:16px;font-size:16px;line-height:24px}
</style>
<div class="sample" style="top:10px">Readable web text — proportional sans</div>
<div class="sample" style="top:42px;font-family:serif;font-size:24px">Serif: a proper heading</div>
<div class="sample" style="top:84px;font-weight:bold">Bold has its own glyphs</div>
<div class="sample" style="top:114px;font-style:italic">Italic, without stamping text twice</div>
<div class="sample" style="top:146px;font-family:monospace">Monospace: code = 123;</div>
<div class="sample" style="top:176px">Україна: ї є ґ. Ελληνικά. Café é.</div>
<div class="sample" style="top:212px;background:#000;color:#fff;width:560px;height:40px">White on black: smooth edges</div>
<div class="sample" style="top:258px;color:#000">iiii WWWW — compare the widths</div>
<a class="sample" style="top:300px" href="/next.html">Follow this link</a>
'''


def main():
    out=ROOT/'build/netsurf/fonts/gui';out.mkdir(parents=True,exist_ok=True)
    build=ROOT/'fuse-x/build';apps={n:build/'apps'/(n+'.fxe') for n in ('nano-X','nxhostcheck','kill')}
    apps['netsurf']=ROOT/'build/netsurf/netsurf.fxe'
    with tempfile.TemporaryDirectory(prefix='font-gui-',dir='/tmp') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'event'
        old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox),NANOX_KEEP_TTY='1')
        files=[dict(name='/dev/gfx',mode=0o20600,device=0x400),
            dict(name='/usr/share/netsurf/web.nfa',path=str(ROOT/'ports/netsurf/fonts/web.nfa')),
            dict(name='/fonts.html',text=PAGE),dict(name='/next.html',text='<!doctype html><html><head><title>Font link passed</title></head><body>Success</body></html>')]
        try:g=Guest(work,'native',apps,files,blocks=65535)
        finally:os.environ.clear();os.environ.update(old)
        def wait(test):
            until=time.monotonic()+60
            while time.monotonic()<until:
                g.pump();value=test()
                if value:return value
            raise AssertionError(g.log[-5000:].decode(errors='replace'))
        def frame(width):
            try:
                item=next(w for w in json.loads((frames/'windows.json').read_text()) if w['visible'] and w['title']=='NetSurf: Readable fonts')
                data=(frames/f"{item['id']}.ppm").read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                w,h=map(int,m.groups());rgb=data[m.end():]
                if w==width and len(rgb)==w*h*3:
                    png(out/'latest.png',w,h,[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
                    if (rgb[(280*w+20)*3:(280*w+20)*3+3]==b'\0\0\0' and
                        all(abs(rgb[(2*w+w-30)*3+i]-c)<9 for i,c in enumerate((44,55,65))) and
                        sum(max(rgb[(y*w+x)*3:(y*w+x)*3+3])>150 for y in range(7,24) for x in range(w-88,w-20))>30):return item,w,h,rgb
            except (OSError,StopIteration,ValueError,AttributeError):pass
        def event(w,**values):
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp');tmp.write_text(json.dumps(dict(window=w,**values)));tmp.replace(mailbox);wait(lambda:not mailbox.exists())
        try:
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('s=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            start=time.monotonic();g.shell('netsurf --no-disk-cache --trace /fonts.html &');g.shell('p=$!')
            wait(lambda:b'NETSURF_GUI_LOADED Readable fonts' in g.log)
            print(f'Cold browser startup through font-page rendering: {time.monotonic()-start:.2f}s',flush=True)
            for width in (600,340,900,600):
                if width!=600:event(item['id'],resize=[width,480])
                elif 'item' in locals() and previous!=600:event(item['id'],resize=[600,480])
                item,w,h,rgb=wait(lambda:frame(width));previous=width
                # Check actual alpha blending on black rather than the CSS
                # text background hint (which would leave pale rectangles).
                colours=set(tuple(rgb[(y*w+x)*3:(y*w+x)*3+3]) for y in range(64+212,min(h-22,64+250)) for x in range(16,min(w-20,320)))
                gray=[c for c in colours if abs(c[0]-c[1])<8 and abs(c[1]-c[2])<8 and 15<c[0]<240]
                assert len(gray)>8,('missing antialias coverage',width,len(gray),colours)
                assert (0,0,0) in colours and (255,255,255) in colours
                for top,bottom in ((74,104),(106,144),(148,176),(178,206),(210,236),(240,268)):
                    assert sum(rgb[(y*w+x)*3:(y*w+x)*3+3]!=b'\xff\xff\xff' for y in range(top,bottom) for x in range(16,min(w-20,280)))>50
                png(out/f'fonts-{width}.png',w,h,[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
            event(item['id'],input=dict(type='button',button=0,down=True,x=60,y=376))
            event(item['id'],input=dict(type='button',button=0,down=False,x=60,y=376))
            wait(lambda:b'NETSURF_GUI_LOADED Font link passed' in g.log)
            wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',g.log)[-1]);g.shell(f'nxhostcheck ctl close {wid}')
            assert 'FONT_GUI_STATUS:0' in g.shell('wait $p; echo FONT_GUI_STATUS:$?')
            assert not any(marker in g.log for marker in (b'GsError',b'panic:',b'NXR_BUFFER_FAILED'))
            g.shell('kill $s; wait $s')
        finally:
            (out/'guest.log').write_bytes(g.log)
            try:g.close()
            finally:(out/'guest.log').write_bytes(g.log)
    print('Guest fonts: sans/serif/mono, weight/italic, UTF-8, dark alpha, resize, link hit testing and clean shutdown passed')


if __name__=='__main__':main()
