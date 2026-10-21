#!/usr/bin/env python3
"""Compare retained rendering to uncached pixels on a disposable Fuse-X disk.

Uses real host-input events and Nano-X surfaces, without a physical display.
Also checks decoded-image eviction/recovery under an intentionally small budget.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT/'libraries/tests'))
from image_fixtures import chunk, zlib


def picture(w, h, colour):
    import struct
    return (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('!IIBBBBB',w,h,8,6,0,0,0))+
            chunk(b'IDAT', zlib.compress((b'\0'+bytes(colour)*w)*h))+chunk(b'IEND',b''))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/netsurf')
    p.add_argument('--graphics-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--sdk',type=Path,help='matching shared-library SDK, if separate')
    p.add_argument('--pages',nargs='+',choices=('page','fixed','long','evict'),default=('page','fixed','long','evict'))
    p.add_argument('--output',type=Path,default=ROOT/'build/netsurf/rendering.json')
    args=p.parse_args();args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='netsurf-render-check-',dir='/tmp') as folder:
        work=Path(folder);frames=work/'frames';events=work/'event.json'
        assets={
            'large.png':picture(1024,512,(210,70,30,255)),
            'alpha.png':picture(4,2,(255,255,255,128)),
        }
        sections=''.join(f'<section style="background:#{r:02x}{g:02x}{b:02x}">'
                         f'<h2>Section {i}: retained document pixels</h2>'
                         '<p>Clipped text, backgrounds and translucent images.</p>'
                         '<img src="alpha.png" width="320" height="64"></section>'
                         for i,(r,g,b) in enumerate([(32,64,96),(70,90,40),(110,60,80),(20,90,110)]*4))
        assets['page.html']=('''<!doctype html><title>Render check</title><style>
            body{margin:0;background:#204060;color:white;width:1100px}
            section{height:256px;padding:8px;box-sizing:border-box}
            h2{margin:0}p{background-image:url(alpha.png)}
            </style><img src="large.png" width="512" height="256">'''+sections).encode()
        assets['fixed.html']=assets['page.html'].replace(b'</style>',b'.fixed{position:fixed;top:20px;left:20px;background:red}</style>').replace(b'<img src="large.png"',b'<div class="fixed">Fixed content</div><img src="large.png"',1)
        assets['long.html']=b'<!doctype html><title>Render check</title><body style="margin:0;background:#204060"><div style="height:32700px"></div>Bottom'
        assets['evict.html']=b'<!doctype html><title>Render check</title><body style="margin:0;background:#204060">'+b''.join(
            f'<div style="height:500px"><img src="{i}.png" width="128" height="128"></div>'.encode() for i in range(3))
        for i,c in enumerate([(255,0,0,255),(0,255,0,255),(0,0,255,255)]):assets[f'{i}.png']=picture(128,128,c)
        files=[dict(name='/dev/gfx',mode=0o20600,device=0x400),
               dict(name='/usr/share/netsurf/web.nfa',path=str(ROOT/'ports/netsurf/fonts/web.nfa'))]
        for name,data in assets.items():
            path=work/name;path.write_bytes(data);files.append(dict(name='/'+name,path=str(path)))
        if args.sdk:
            files += [dict(name='/lib/fuse-x/'+f.name,path=str(f.resolve())) for f in (args.sdk/'lib').glob('*.fxl.[0-9]')]
        apps={n:args.build.resolve()/(n+'.fxe') for n in ('netsurf','htget')}
        apps.update({n:args.graphics_build.resolve()/'apps'/(n+'.fxe') for n in ('nano-X','nxhostcheck')})
        apps['kill']=ROOT/'fuse-x/build/apps/kill.fxe'
        previous=dict(os.environ)
        os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(events),NANOX_KEEP_TTY='1')
        try:g=Guest(work,'native',apps,files,blocks=131072)
        finally:os.environ.clear();os.environ.update(previous)

        def wait(predicate,description,seconds=60):
            deadline=time.monotonic()+seconds
            while not predicate():
                g.pump()
                if time.monotonic()>deadline:raise AssertionError(description+'\n'+g.log[-3500:].decode(errors='replace'))

        def snapshot(size=None):
            registry=json.loads((frames/'windows.json').read_text())
            item=next(w for w in registry if w['visible'] and w['title']=='NetSurf: Render check')
            data=(frames/f"{item['id']}.ppm").read_bytes()
            m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data);w,h=map(int,m.groups());rgb=data[m.end():]
            assert len(rgb)==w*h*3 and (not size or size==(w,h))
            return item,w,h,rgb

        def settled(size=None,after_sequence=None):
            deadline=time.monotonic()+30;last=None;since=time.monotonic()
            while time.monotonic()<deadline:
                g.pump()
                try:
                    value=snapshot(size)
                    if after_sequence is not None and value[0]['sequence']<=after_sequence:continue
                    if value[1:]!=last:last=value[1:];since=time.monotonic()
                    if time.monotonic()-since>.12:return value
                except (OSError,StopIteration,ValueError,AssertionError,AttributeError):pass
            raise AssertionError('no settled frame')

        def event(item,**command):
            temporary=events.with_suffix('.tmp');temporary.write_text(json.dumps(dict(window=item['id'],**command)));temporary.replace(events)
            wait(lambda:not events.exists(),'host event consumed')

        def key(item,hid):
            event(item,input=dict(type='key',key=hid,down=True))
            event(item,input=dict(type='key',key=hid,down=False))

        rows=[];saved={}
        try:
            g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY');g.shell('nano-X -p &');g.shell('s=$!')
            if b'Nano-X ready' not in g.log:g.read(b'Nano-X ready')
            for page in args.pages:
                for cached in (False,True):
                    begin=len(g.log)
                    limits='NETSURF_IMAGE_CACHE_PIXELS=32768 NETSURF_IMAGE_PIXELS=16384 ' if page=='evict' else ''
                    g.shell(limits+'NETSURF_TRACE_IMAGES=1 netsurf --trace --images '+('' if cached else '--no-render-cache ')+f'/{page}.html &');g.shell('p=$!')
                    wait(lambda:b'NETSURF_GUI_LOADED Render check' in g.log[begin:],'document loaded',180)
                    wid=int(re.findall(rb'NETSURF_GUI_READY (\d+)',g.log[begin:])[-1])
                    state=settled();samples=[];latencies=[]
                    for stage in range(12):
                        if stage:
                            before=len(g.log);start=time.monotonic();item=state[0]
                            marker=b'NETSURF_GUI_SCROLL '
                            if stage==6:
                                event(item,resize=[780,500])
                                marker=b'NETSURF_GUI_RESIZE 780 500'
                            elif stage==7:key(item,77);marker=b'NETSURF_GUI_KEY 63495 ' # End
                            elif stage in (8,11):key(item,74);marker=b'NETSURF_GUI_KEY 63494 ' # Home
                            elif stage==9:key(item,62);marker=b'NETSURF_GUI_KEY 63519 ' # F5
                            elif stage==10:key(item,77);marker=b'NETSURF_GUI_KEY 63495 '
                            elif stage==4 and page in ('page','fixed'):key(item,79);marker=b'NETSURF_GUI_KEY 63489 ' # Right
                            else:event(item,input=dict(type='wheel',x=0,y=-65536))
                            wait(lambda:marker in g.log[before:] and b'NETSURF_GUI_PAINT' in g.log[before:].split(marker,1)[1],'paint after input')
                            if stage==9:wait(lambda:b'NETSURF_GUI_DOCUMENT' in g.log[before:],'reload completed')
                            state=settled((780,500) if stage>=6 else None,item['sequence']);latencies.append(time.monotonic()-start)
                        _,w,h,rgb=state
                        if page=='evict' and stage in (0,8,11):
                            offset=(96*w+32)*3
                            assert rgb[offset:offset+3]==bytes((255,0,0)), 'evicted first image must recover its red pixels'
                        pixels=b''.join(rgb[(y*w)*3:(y*w+w-14)*3] for y in range(64,h-22))
                        samples.append(hashlib.sha256(pixels).hexdigest())
                        if not cached:saved[page,stage]=pixels
                        else:
                            baseline=saved[page,stage]
                            if pixels!=baseline:
                                for name,data in [('baseline',baseline),('cached',pixels)]:
                                    args.output.with_name(f'{args.output.stem}-{page}-{stage}-{name}.ppm').write_bytes(
                                        f'P6\n{w-14} {h-86}\n255\n'.encode()+data)
                            assert pixels==baseline,(page,stage,sum(a!=b for a,b in zip(pixels,baseline)))
                    if page=='page':
                        reference=pixels
                        item=state[0]
                        g.shell(f'nxhostcheck ctl hide {wid}');g.shell(f'nxhostcheck ctl show {wid}')
                        state=settled(after_sequence=item['sequence'])
                        item=state[0]
                        event(item,input=dict(type='button',button=0,down=True,x=740,y=16))
                        event(item,input=dict(type='button',button=0,down=False,x=740,y=16))
                        state=settled(after_sequence=item['sequence']);before=len(g.log)
                        key(state[0],41) # Escape dismisses the overlay.
                        marker=b'NETSURF_GUI_KEY 27 '
                        wait(lambda:marker in g.log[before:] and b'NETSURF_GUI_PAINT' in g.log[before:].split(marker,1)[1],'menu dismissal repaint')
                        _,w,h,rgb=settled(after_sequence=state[0]['sequence'])
                        assert b''.join(rgb[(y*w)*3:(y*w+w-14)*3] for y in range(64,h-22))==reference,'exposure/menu must restore document pixels'
                    log=bytes(g.log[begin:])
                    if cached and page in ('page','long'):assert b'NETSURF_RENDER ' in log
                    if cached and page=='fixed':assert b'NETSURF_RENDER ' not in log,'fixed boxes must use conservative repaint'
                    if page=='evict':assert b'NETSURF_IMAGE_EVICT' in log
                    if page=='page':assert re.search(rb'large.png octets=\d+ status=0',log),'1024px PNG decoding'
                    renders=[dict(zip(('hits','misses','raster','reused','ms'),map(int,m))) for m in re.findall(rb'NETSURF_RENDER hit=(\d+) miss=(\d+) raster=(\d+) reused=(\d+) ms=(\d+)',log)]
                    paint_ms=list(map(int,re.findall(rb'NETSURF_GUI_PAINT .*?ms=(\d+)',log)))
                    rows.append(dict(page=page,cached=cached,hashes=samples,settled_frame_seconds=latencies,renders=renders,paint_ms=paint_ms))
                    g.shell(f'nxhostcheck ctl close {wid}');assert 'RENDER_EXIT:0' in g.shell('wait $p; echo RENDER_EXIT:$?')
                    print(page,'cached' if cached else 'baseline','passed',flush=True)
            g.shell('kill $s; wait $s')
        finally:
            args.output.with_suffix('.log').write_bytes(g.log)
            g.close()
        assert not any(v in g.log for v in (b'GsError',b'panic:',b'NXR_BUFFER_FAILED',b'blit snapshot'))
        args.output.write_text(json.dumps(rows,indent=2)+'\n')
    print('Native rendering: pixel equivalence, scrolling, resize, reload, fixed fallback and image eviction passed')


if __name__=='__main__':main()
