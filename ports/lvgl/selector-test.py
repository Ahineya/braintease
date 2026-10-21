#!/usr/bin/env python3
"""Actual C16 selector acceptance on a fresh, offline, explicit-manifest disk."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
ROOT=Path(__file__).resolve().parents[2]
PORT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
spec=importlib.util.spec_from_file_location('fixtures',PORT/'selector-fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)

def ppm(path):
    data=path.read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
    if not m:return None
    w,h=map(int,m.groups());pixels=data[m.end():]
    return (w,h,pixels) if len(pixels)==w*h*3 else None

def main():
    build=ROOT/'build/lvgl';software='--software-upload' in sys.argv;scale=2 if '--retina' in sys.argv else 1
    legacy=software or '--rgb565' in sys.argv
    output=build/(('selector-tests-software' if software else 'selector-tests-rgb565' if legacy else 'selector-tests')+('-2x' if scale==2 else ''));output.mkdir(exist_ok=True)
    (output/'result.json').unlink(missing_ok=True)
    folder,files=fixtures.prepare()
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'browser-test.c'),str(PORT/'browser.c'),
                    '--static','-o',str(output/'browser-test.fxe')],cwd=ROOT,check=True)
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'upload-check.c'),
                    '-I',str(ROOT/'ports/graphics/include'),'--static','-o',str(output/'upload-check.fxe')],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='lvselect-guest-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'input.json'
        saved=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{'lvselect':build/'lvselect.fxe','browser-model':output/'browser-test.fxe','upload-check':output/'upload-check.fxe'},
                         [dict(name='/dev/gfx',mode=0o20600,device=0x400),*files],blocks=32768)
        finally:os.environ.clear();os.environ.update(saved)
        def wait(test,seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                g.pump();value=test()
                if value:return value
                assert g.process.poll() is None,g.log[-6000:].decode(errors='replace')
            raise AssertionError('selector wait timed out:\n'+g.log[-6000:].decode(errors='replace'))
        def pause(seconds=.15):
            until=time.monotonic()+seconds
            while time.monotonic()<until:g.pump()
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def windows():return [w for w in registry() if w['visible'] and w['title']=='Select File']
        def event(wid,**values):
            # Enter/Escape can finish the app on key-down before its release
            # arrives. Do not leave an obsolete release in the host mailbox.
            if not any(w['id']==wid for w in registry()):return
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp')
            tmp.write_text(json.dumps(dict(window=wid,**values)));tmp.replace(mailbox)
            wait(lambda:not mailbox.exists() or not any(w['id']==wid for w in registry()))
            if mailbox.exists():mailbox.unlink(missing_ok=True)
        def key(wid,code):
            event(wid,input=dict(type='key',key=code,down=True));event(wid,input=dict(type='key',key=code,down=False))
        def text(wid,s):
            for c in s:event(wid,input=dict(type='text',scalar=ord(c)))
        def click(wid,x,y):
            event(wid,input=dict(type='button',button=0,down=True,x=x,y=y))
            event(wid,input=dict(type='button',button=0,down=False,x=x,y=y))
        def logged(marker,at):return wait(lambda:marker.encode() in g.log[at:])
        def frame(wid,w=640,h=480):
            def read():
                try:
                    v=ppm(frames/f'{wid}.ppm')
                    return v if v and v[:2]==(w*scale,h*scale) and v[2][:3]==bytes((24,24,24) if legacy else (25,26,28)) else None
                except FileNotFoundError:return None
            return wait(read)
        def ctrl_l(wid):
            event(wid,input=dict(type='key',key=224,down=True));key(wid,15)
            event(wid,input=dict(type='key',key=224,down=False))
        def path(wid,value):
            ctrl_l(wid);event(wid,action=2,clipboard=value)
        def path_pixels(wid):
            pause(.6)
            w,h,pixels=frame(wid)
            return b''.join(pixels[(y*w+14*scale)*3:(y*w+554*scale)*3]
                            for y in range(46*scale,72*scale))
        def launch(name='one'):
            prefix=f'LVGL_SCALE={scale} LVGL_PROFILE=1 '+('LVGL_SOFTWARE_UPLOAD=1 ' if software else '')
            if legacy:prefix+='LVGL_RGB565=1 '
            g.shell(f'{prefix}lvselect --trace {folder} > /tmp/{name} &');g.shell(f'{name}=$!')
            wait(lambda:len(windows())==1);wid=windows()[0]['id'];frame(wid);pause(.6);return wid
        def finish(wid,name,expected,close=False):
            if close:event(wid,close=True)
            wait(lambda:not windows());pause(.1)
            assert 'STATUS:0' in g.shell(f'wait ${name}; echo STATUS:$?')
            answer=g.shell(f'cat /tmp/{name}; echo END')
            assert '\n'+expected+'\n' in answer,repr(answer)
            wait(lambda:not windows())
        try:
            assert 'BROWSER_MODEL passed' in g.shell(f'browser-model {folder}')
            assert 'UPLOAD_CHECK passed' in g.shell('upload-check')
            # Upload must preserve neighbouring pixels and ignore row padding.
            image=ppm(frames/'1.ppm');assert image[:2]==(4,3)
            colour=[tuple(image[2][i:i+3]) for i in range(0,len(image[2]),3)]
            assert colour==[(0,0,0)]*5+[(255,0,0),(0,255,0),(0,0,0),(0,0,0),(0,0,255),(255,255,255),(0,0,0)],colour
            image=ppm(frames/'2.ppm');assert image[:2]==(4,3)
            colour=[tuple(image[2][i:i+3]) for i in range(0,len(image[2]),3)]
            assert colour==[(1,2,3),(4,5,6),(34,35,36),(34,35,36),
                            (7,8,9),(10,11,12),(4,5,6),(34,35,36),
                            (34,35,36),(7,8,9),(10,11,12),(34,35,36)],colour
            wid=launch()
            actual=frame(wid);reference=ppm(build/('host/selector-2x.ppm' if scale==2 else 'host/selector.ppm'))
            assert actual[:2]==reference[:2]
            assert windows()[0]['format']==(1 if legacy else 2),'Unexpected selected surface format'
            def rgb565(p):return ((p[0]>>3)<<11)|((p[1]>>2)<<5)|(p[2]>>3)
            equal=sum((rgb565(actual[2][i:i+3])==rgb565(reference[2][i:i+3]) if legacy else actual[2][i:i+3]==reference[2][i:i+3]) for i in range(0,len(actual[2]),3))
            total=len(actual[2])//3
            shutil.copyfile(frames/f'{wid}.ppm',output/'initial.ppm')
            (output/'pixels.json').write_text(json.dumps(dict(equal=equal,total=total,fraction=equal/total),indent=2)+'\n')
            assert equal==total,f'Guest/reference pixel mismatch: {equal}/{total}'
            # This is shared desktop focus policy, exercised outside the gallery.
            # Blurring must clear selection and stop both keys and native paste.
            baseline=path_pixels(wid)
            for point in ((360,97),(400,20)):
                ctrl_l(wid);click(wid,*point);at=len(g.log)
                text(wid,'unwanted');event(wid,action=2,clipboard='/unwanted');key(wid,42);key(wid,40);pause(.3)
                assert not any(v in g.log[at:] for v in (b'SELECT_PATH ',b'SELECT_DIR ',b'SELECT_ERROR ')),'Blurred path field handled editing'
                assert path_pixels(wid)==baseline,'Path kept selection, caret or focus border after blur'
            # Tab starts with the path control. Its old select-all replacement
            # mode must not survive a background click and refocus.
            at=len(g.log);key(wid,43);key(wid,77);text(wid,'!');logged(f'SELECT_PATH {folder}!',at)
            key(wid,42);key(wid,81)
            # Directories first, keyboard enter/right, parent and path canonicalization.
            at=len(g.log);key(wid,81);logged('SELECT_ROW 1 Alpha',at)
            at=len(g.log);key(wid,79);logged(f'SELECT_DIR {folder}/Alpha count=2',at)
            at=len(g.log);click(wid,44,97);logged(f'SELECT_DIR {folder} count=68',at)
            logged('SELECT_ROW 1 Alpha',at)
            at=len(g.log);key(wid,78);logged('SELECT_ROW 10 track-002.txt',at)
            at=len(g.log);key(wid,77);logged('SELECT_ROW 67 track-059.txt',at)
            at=len(g.log);key(wid,74);logged('SELECT_ROW 0 ..',at)
            # Wheel under the pointer, including fractional high-resolution deltas.
            event(wid,input=dict(type='motion',x=400,y=280));at=len(g.log)
            for _ in range(4):event(wid,input=dict(type='wheel',x=0,y=-16384))
            logged('SELECT_SCROLL ',at)
            wait(lambda:any(int(n)>0 for n in re.findall(rb'SELECT_SCROLL (-?\d+)',g.log[at:])))
            # Shift-Tab leaves the list for the native checkbox; Enter toggles it.
            path_before=path_pixels(wid)
            event(wid,input=dict(type='key',key=225,down=True));key(wid,43)
            event(wid,input=dict(type='key',key=225,down=False));at=len(g.log);key(wid,40)
            logged(f'SELECT_DIR {folder} count=69 hidden=1',at)
            assert path_pixels(wid)==path_before,'Path text moved when showing hidden files'
            at=len(g.log);click(wid,560,97);logged(f'SELECT_DIR {folder} count=68 hidden=0',at)
            assert path_pixels(wid)==path_before,'Path text moved when hiding hidden files'
            for hidden in (1,0):
                at=len(g.log);click(wid,560,97)
                logged(f'SELECT_DIR {folder} count={68+hidden} hidden={hidden}',at)
                assert path_pixels(wid)==path_before,'Path text drifted after repeated toggles'
            # Invalid and oversized pastes preserve the usable listing.
            path(wid,str(folder/'missing'));at=len(g.log);key(wid,40);logged('SELECT_ERROR Cannot open path:',at)
            event(wid,action=4);pause(.1);oversized_before=path_pixels(wid)
            event(wid,action=2,clipboard='x'*2048);pause(.1)
            assert path_pixels(wid)==oversized_before,'Oversized paste changed the path field'
            path(wid,str(folder/'empty'));at=len(g.log);key(wid,40);logged(f'SELECT_DIR {folder}/empty count=1',at)
            at=len(g.log);key(wid,42);logged(f'SELECT_DIR {folder} count=68',at)
            g.shell(f'HOME={folder}/empty; export HOME')
            # Existing processes inherit their starting HOME; test it with a
            # later instance below rather than changing this selector's state.
            # Refresh is asynchronous and preserves the selected row.
            key(wid,81);at=len(g.log);key(wid,62);logged(f'SELECT_DIR {folder} count=68',at);logged('SELECT_ROW 1 Alpha',at)
            # Responsive layout and bounded guest draw buffers.
            for w,h,accepted in [(900,700,(900,700)),(20,20,(480,340)),(1600,1200,(1600,1080)),(640,480,(640,480))]:
                event(wid,resize=[w,h]);frame(wid,*accepted)
            # Native clipboard and Unicode committed text preserve filesystem octets.
            path(wid,str(folder)+'/caf');text(wid,'é');key(wid,42);text(wid,'éX');key(wid,42);text(wid,'.txt')
            event(wid,action=1);event(wid,action=3);event(wid,action=2)
            pause(.7);shutil.copyfile(frames/f'{wid}.ppm',output/'selection.ppm')
            before=(frames/f'{wid}.ppm').stat().st_mtime_ns;pause(1.3)
            shutil.copyfile(frames/f'{wid}.ppm',output/'idle-after.ppm')
            assert (frames/f'{wid}.ppm').stat().st_mtime_ns==before,'Idle selector keeps publishing frames'
            key(wid,40);finish(wid,'one',str(folder/'café.txt'))
            # Double-click a directory, then a file. stdout remains protocol-only.
            wid=launch('two');click(wid,100,166);at=len(g.log);click(wid,100,166)
            logged(f'SELECT_DIR {folder}/Alpha count=2',at);pause(.4)
            click(wid,100,166);click(wid,100,166);finish(wid,'two',str(folder/'Alpha/nested.txt'))
            wid=launch('three');at=len(g.log);click(wid,116,97)
            logged(f'SELECT_DIR {folder}/empty count=1',at)
            key(wid,41);finish(wid,'three','[]')
            wid=launch('four');finish(wid,'four','[]',close=True)
            assert 'BAD_STATUS:1' in g.shell('lvselect /no-such-folder; echo BAD_STATUS:$?')
            profiles=re.findall(rb'\bdirect_upload=(\d+)',g.log)
            assert len(profiles)==4 and all(int(p)==int(not software) for p in profiles),profiles
            assert not any(e in g.log for e in (b'native runtime error',b'panic:',b'Assertion `',b'graphics operation failed'))
            report=dict(status='passed',scale=scale,format='RGB565' if legacy else 'XRGB8888',direct_upload=not software,pixels_equal=equal,pixels_total=total,
                        executable_sha256=hashlib.sha256((build/'lvselect.fxe').read_bytes()).hexdigest(),
                        checks=['guest filesystem model','exact host/guest pixels','directories and parent',
                                'arrows and paging','wheel fractions','Tab and checkbox','blank-panel blur and refocus','stable path baseline on toggles','refresh',
                                'invalid path preservation','UTF-8 clipboard and editing','resize bounds',
                                'idle damage','double-click selection','stdout protocol','cancel and close','filesystem cleanup'])
        finally:
            try:g.close()
            finally:(output/'guest.log').write_bytes(g.log)
        (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('LVGL/Fuse-X selector: guest rendering, browsing, scrolling, clipboard, keyboard and lifecycle passed')

if __name__=='__main__':main()
