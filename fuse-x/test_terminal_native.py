#!/usr/bin/env python3
"""Measure real hosted terminal resizes with immutable published-frame captures."""
import json
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def run():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'fuse-x/build')
    args=parser.parse_args();build=args.build.resolve()
    with tempfile.TemporaryDirectory(prefix='fuse-x-terminal-',dir='/tmp') as temporary:
        work=Path(temporary);frames=work/'frames';event=work/'event.json'
        application=work/'render.fxe'
        subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(ROOT/'fuse-x/examples/terminal-render.c'),
                        '--build',str(build),'-o',str(application)],check=True)
        os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(event),FUSE_X_TERM_TRACE='1')
        # A non-antialiased sixteen-colour frame can contain only 0 and 168
        # channel values; native text has many intermediate alpha blends.
        minimum_colours=1 if os.environ.get('FUSE_X_TERM_STYLE')=='classic' else 4
        apps={'gterm':build/'apps/gterm.fxe','render':application}
        files=[]
        for i in range(16):
            files.extend([dict(name=f'/dev/ptyp{i:x}',mode=0o20600,device=0x300+i),
                          dict(name=f'/dev/ttyp{i:x}',mode=0o20600,device=0x202+i)])
        files.append(dict(name='/dev/gfx',mode=0o20666,device=0x400))
        files.extend(dict(name='/lib/fuse-x/'+path.name,path=str(path)) for path in (build/'lib').glob('*.fxl.[0-9]'))
        guest=Guest(work,'native',apps,files,blocks=24576,build_override=build)
        try:
            guest.shell('gterm render &')
            def windows():
                try:return json.loads((frames/'windows.json').read_text())
                except FileNotFoundError:return []
            def wait(condition):
                end=time.monotonic()+30
                while time.monotonic()<end:
                    guest.pump()
                    value=condition()
                    if value:return value
                raise AssertionError(guest.log[-4000:].decode(errors='replace'))
            window=wait(lambda:next((w for w in windows() if w['title']=='Terminal'),None))
            wid=window['id'];cw,ch=window['width']//80,window['height']//30
            print(f'Native terminal cell geometry: {cw}x{ch}',flush=True)
            def published(width,height):
                try:
                    data=(frames/f'{wid}.ppm').read_bytes()
                    return data if data.startswith(f'P6\n{width} {height}\n255\n'.encode()) and len(set(data[-width*height*3:]))>minimum_colours else None
                except FileNotFoundError:return None
            wait(lambda:published(80*cw,30*ch))
            for cols,rows in ((160,55),(180,60),(100,30)):
                width,height=cols*cw,rows*ch
                before=time.monotonic()
                temporary=event.with_suffix('.tmp');temporary.write_text(json.dumps(dict(window=wid,resize=[width,height])))
                temporary.replace(event)
                frame=wait(lambda:published(width,height));elapsed=time.monotonic()-before
                assert len(set(frame[-width*height*3:]))>minimum_colours
                assert elapsed<2.0,(cols,rows,elapsed)
                print(f'Guest resize {cols}x{rows}: {elapsed*1000:.1f} ms (includes RGB screenshot capture)',flush=True)
            command=event.with_suffix('.tmp');command.write_text(json.dumps(dict(window=wid,close=True)));command.replace(event)
            wait(lambda:not windows())
            print('Native terminal: real guest resize, font metrics, coloured frames and close passed',flush=True)
        finally:guest.close()


if __name__=='__main__':run()
