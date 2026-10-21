#!/usr/bin/env python3
"""Check automatic backing-density transitions using real guest apps."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
spec=importlib.util.spec_from_file_location('fixtures',PORT/'selector-fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)

def main():
    build=ROOT/'build/lvgl';out=build/'retina-tests';out.mkdir(exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    folder,files=fixtures.prepare()
    with tempfile.TemporaryDirectory(prefix='lvgl-retina-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'event.json'
        env=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{a:build/(a+'.fxe') for a in ('lvcalc','lvselect')},
                         [dict(name='/dev/gfx',mode=0o20600,device=0x400),*files],blocks=65536)
        finally:os.environ.clear();os.environ.update(env)
        def wait(check,seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                g.pump();value=check()
                if value:return value
                assert g.process.poll() is None,g.log[-4000:].decode(errors='replace')
            raise AssertionError('Retina wait timed out:\n'+g.log[-5000:].decode(errors='replace'))
        def pause(n=.15):
            end=time.monotonic()+n
            while time.monotonic()<end:g.pump()
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def window(title):return wait(lambda:next((w for w in registry() if w['title']==title),None))
        def alive(wid):return any(w['id']==wid for w in registry())
        def event(wid,**values):
            if not alive(wid):return
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp')
            tmp.write_text(json.dumps(dict(window=wid,**values)));tmp.replace(mailbox)
            wait(lambda:not mailbox.exists() or not alive(wid))
            if mailbox.exists():mailbox.unlink(missing_ok=True)
        def key(wid,code):
            event(wid,input=dict(type='key',key=code,down=True));event(wid,input=dict(type='key',key=code,down=False))
        def click(wid,x,y):
            event(wid,input=dict(type='button',button=0,down=True,x=x,y=y));event(wid,input=dict(type='button',button=0,down=False,x=x,y=y))
        def text(wid,value):
            for c in value:event(wid,input=dict(type='text',scalar=ord(c)))
        def frame(wid,w,h,scale):
            def check():
                item=next((v for v in registry() if v['id']==wid),None)
                if not item or (item['width'],item['height'],item['scale'])!=(w,h,scale):return None
                try:data=(frames/f'{wid}.ppm').read_bytes()
                except FileNotFoundError:return None
                m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                return data if m and tuple(map(int,m.groups()))==(w*scale,h*scale) and item['format']==2 and data[m.end():m.end()+3]==bytes((25,26,28)) else None
            wait(check);pause(.5)
        try:
            g.shell('unset LVGL_SCALE')
            g.shell(f'lvselect --trace {folder} > /tmp/choice &');g.shell('selector=$!')
            wid=window('Select File')['id'];event(wid,density=2);frame(wid,640,480,2)
            event(wid,input=dict(type='key',key=224,down=True));key(wid,15)
            event(wid,input=dict(type='key',key=224,down=False));event(wid,action=2,clipboard=str(folder)+'/A ')
            text(wid,'Україна');key(wid,42);text(wid,'а.txt')
            for scale in (1,2,1,2):event(wid,density=scale);frame(wid,640,480,scale)
            shutil.copyfile(frames/f'{wid}.ppm',out/'selector-2x.ppm')
            # A logical click must still hit Go after multiple density changes.
            click(wid,596,59);wait(lambda:not alive(wid));pause()
            assert 'STATUS:0' in g.shell('wait $selector; echo STATUS:$?')
            assert '\n'+str(folder/'A Україна.txt')+'\n' in g.shell('cat /tmp/choice; echo END')
            g.shell('lvcalc --trace &');g.shell('calculator=$!');wid=window('Calculator')['id']
            event(wid,density=2);frame(wid,300,360,2);text(wid,'12+3')
            for scale in (1,2):event(wid,density=scale);frame(wid,300,360,scale)
            at=len(g.log);click(wid,256,340);wait(lambda:b'CALC_RESULT 15' in g.log[at:])
            event(wid,resize=[700,600]);frame(wid,700,600,2)
            event(wid,density=1);frame(wid,700,600,1)
            event(wid,resize=[300,360]);frame(wid,300,360,1)
            event(wid,density=2);frame(wid,300,360,2)
            shutil.copyfile(frames/f'{wid}.ppm',out/'calculator-2x.ppm')
            event(wid,close=True);wait(lambda:not alive(wid));pause()
            assert 'STATUS:0' in g.shell('wait $calculator; echo STATUS:$?')
            # Fixed 1x remains fixed even when the display prefers 2x.
            g.shell('LVGL_SCALE=1 lvcalc &');g.shell('fixed=$!');wid=window('Calculator')['id']
            frame(wid,300,360,1);event(wid,density=2);pause(.3);frame(wid,300,360,1)
            event(wid,close=True);wait(lambda:not alive(wid));pause()
            assert 'STATUS:0' in g.shell('wait $fixed; echo STATUS:$?')
            assert not any(e in g.log for e in (b'native runtime error',b'panic:',b'Assertion `',b'graphics operation failed'))
            report=dict(status='passed',checks=['automatic 1x/2x transitions','Ukrainian committed input and deletion',
                'path preserved across transitions','logical mouse hit testing','calculator expression preserved',
                'resize combined with density changes','fixed-scale override','clean shutdown'],
                executables={a:hashlib.sha256((build/(a+'.fxe')).read_bytes()).hexdigest() for a in ('lvcalc','lvselect')})
        finally:
            try:g.close()
            finally:(out/'guest.log').write_bytes(g.log)
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('LVGL/Fuse-X: Retina transitions, Ukrainian editing, geometry and lifecycle passed')

if __name__=='__main__':main()
