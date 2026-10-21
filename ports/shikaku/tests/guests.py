#!/usr/bin/env python3
"""Shikaku rules, real input/pixels, zoom/resize and saved progress."""
import argparse,json,os,re,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
PUZZLES=json.loads((ROOT/'ports/shikaku/puzzles.json').read_text())

def native(output,check_rules=True):
    output.mkdir(parents=True,exist_ok=True);build=ROOT/'fuse-x/build'
    with tempfile.TemporaryDirectory(prefix='shikaku-native-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'input.json'
        old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        apps={n:build/'apps'/f'{n}.fxe' for n in ('nano-X','shikaku','shikakucheck','kill','nxcheck')}
        extra=[dict(name='/dev/gfx',mode=0o20600,device=0x400)]
        try:g=Guest(work,'native',apps,extra)
        finally:os.environ.clear();os.environ.update(old)
        def wait(test,seconds=90):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                g.pump();result=test()
                if result:return result
                assert g.process.poll() is None,g.log[-6000:].decode(errors='replace')
            raise AssertionError(g.log[-6000:].decode(errors='replace'))
        def window():
            def find():
                try:return next((w for w in json.loads((frames/'windows.json').read_text()) if w['visible'] and w['title'].startswith('Shikaku /')),None)
                except (FileNotFoundError,json.JSONDecodeError):return None
            return wait(find)
        def event(**fields):
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp');tmp.write_text(json.dumps(dict(window=fields.pop('_window',None) or window()['id'],**fields)));tmp.replace(mailbox);wait(lambda:not mailbox.exists())
        def key(code):
            wid=window()['id'];event(_window=wid,input=dict(type='key',key=code,down=True));event(_window=wid,input=dict(type='key',key=code,down=False))
        def marker(text,at=0):return wait(lambda:text.encode() in g.log[at:])
        def painted(at):marker('SHIKAKU_PAINT ',at);time.sleep(.12)
        def geometry():
            values=re.findall(rb'SHIKAKU_PAINT (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\r?\n',g.log)
            assert values;return tuple(map(int,values[-1]))[1:]
        def capture(name):
            def frame():
                try:
                    data=(frames/f"{window()['id']}.ppm").read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    return data if m and len(data)==m.end()+int(m[1])*int(m[2])*3 else None
                except FileNotFoundError:return None
            data=wait(frame);(output/name).write_bytes(data);return data
        def click(x,y,button=0):
            event(input=dict(type='motion',x=x,y=y));event(input=dict(type='button',button=button,down=True,x=x,y=y));event(input=dict(type='button',button=button,down=False,x=x,y=y))
        def toolbar(index):
            w=window()['width'];click(10+index*((w-20)//9)+((w-20)//9)//2,55)
        def drag(r,button=0,reverse=False):
            t,bx,by,_,_=geometry();x,y,w,h,*_=r
            x0=bx+x*t+t//2;y0=by+y*t+t//2;x1=bx+(x+w-1)*t+t//2;y1=by+(y+h-1)*t+t//2
            if reverse:x0,x1,y0,y1=x1,x0,y1,y0
            event(input=dict(type='motion',x=x0,y=y0));event(input=dict(type='button',button=button,down=True,x=x0,y=y0))
            event(input=dict(type='motion',x=x1,y=y1));event(input=dict(type='button',button=button,down=False,x=x1,y=y1))
        try:
            if check_rules:
                result=g.shell('shikakucheck',seconds=240);assert 'failures=0' in result,result
            g.shell('SHIKAKU_TRACE=1; export SHIKAKU_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');marker('Nano-X ready')
            result=g.shell('nxcheck',seconds=60);assert 'NANOX_CLIENT_OK' in result and 'FAILED' not in result,result
            at=len(g.log);g.shell('shikaku 5 &');g.shell('game=$!');marker('SHIKAKU_READY',at);painted(at)
            assert window()['resizable'] is True
            initial=capture('initial.ppm');assert len(set(initial))>12
            first=PUZZLES[0]['solution'][0]
            at=len(g.log);drag(first,reverse=True);marker('SHIKAKU_CHANGE 0 1 1 0',at);painted(at)
            assert capture('rectangle.ppm')!=initial
            at=len(g.log);drag(first,button=1);marker('SHIKAKU_CHANGE 0 0 2 0',at)
            at=len(g.log);key(24);marker('SHIKAKU_CHANGE 0 1 1 0',at)
            at=len(g.log);key(28);marker('SHIKAKU_CHANGE 0 0 2 0',at)
            at=len(g.log);key(24);marker('SHIKAKU_CHANGE 0 1 1 0',at)
            # Ctrl-left drag and Shift-left drag are both erase gestures.
            for modifier in (224,225):
                event(input=dict(type='key',key=modifier,down=True));at=len(g.log);drag(first);event(input=dict(type='key',key=modifier,down=False));marker('SHIKAKU_CHANGE 0 0 2 0',at)
                at=len(g.log);key(24);marker('SHIKAKU_CHANGE 0 1 1 0',at)
            at=len(g.log);key(20);marker('SHIKAKU_EXIT 0',at);g.shell('wait $game')
            at=len(g.log);g.shell('shikaku 5 &');g.shell('game=$!');marker('SHIKAKU_PUZZLE 0 5 1',at);marker('SHIKAKU_READY',at);painted(at)
            # Finish an original puzzle by real drag gestures; no game hooks.
            for r in PUZZLES[0]['solution'][1:]:
                at=len(g.log);drag(r);marker('SHIKAKU_CHANGE ',at);painted(at)
            marker(f'SHIKAKU_SAVE 0 {len(PUZZLES[0]["solution"])} 1');capture('solved.ppm')
            key(21);time.sleep(.1);capture('clear-dialog.ppm');key(41)
            # Resize preserves a completed puzzle, manual zoom, then Fit.
            for w,h in ((900,760),(480,340),(640,540)):
                at=len(g.log);event(resize=[w,h]);marker(f'SHIKAKU_RESIZE {w} {h}',at);painted(at);capture(f'resize-{w}x{h}.ppm')
            at=len(g.log);toolbar(0);time.sleep(.1);capture('sizes.ppm');key(79);key(79);key(81);at=len(g.log);key(40);marker('SHIKAKU_PUZZLE 60 25 0',at);painted(at);capture('25-fit.ppm')
            t0=geometry()[0];at=len(g.log);toolbar(7);wait(lambda:geometry()[0]>t0);painted(at);assert geometry()[0]>t0
            for _ in range(4):
                previous=geometry()[0];at=len(g.log);toolbar(7);wait(lambda:geometry()[0]>previous);painted(at)
            capture('25-zoom.ppm')
            before=capture('before-scroll.ppm');at=len(g.log);key(78);painted(at);assert capture('after-scroll.ppm')!=before
            at=len(g.log);toolbar(8);wait(lambda:geometry()[0]==t0);painted(at);assert geometry()[0]==t0
            # Ordinary lines are dashed; every fifth is solid but a single pixel wide.
            toolbar(0);key(82);at=len(g.log);key(40);marker('SHIKAKU_PUZZLE 24 10 0',at);painted(at)
            data=capture('10-grid.ppm');m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data);w=int(m[1]);pixels=data[m.end():];t,bx,by,_,_=geometry()
            def pixel(x,y):return pixels[(y*w+x)*3:(y*w+x+1)*3]
            assert pixel(bx+5*t,by+3)!=pixel(bx+5*t+1,by+3)
            assert len({pixel(bx+5*t,by+y) for y in range(2,12)})==1
            assert len({pixel(bx+t,by+y) for y in range(2,12)})==2
            at=len(g.log);key(58);time.sleep(.1);capture('help.ppm');key(41);painted(at)
            time.sleep(.4);sequence=window()['sequence']
            for _ in range(20):g.pump();time.sleep(.02)
            assert window()['sequence']==sequence,'idle repaint loop'
            # New makes a seed-based board; progress and identity survive reopening.
            at=len(g.log);toolbar(1);marker('SHIKAKU_SEED 10 ',at);painted(at);capture('generated.ppm')
            seed=re.findall(rb'SHIKAKU_SEED 10 (\d+) (\d+)',g.log[at:])[-1]
            at=len(g.log);drag((0,0,1,1));marker('SHIKAKU_SAVE 24 1 0',at)
            at=len(g.log);key(20);marker('SHIKAKU_EXIT 0',at);g.shell('wait $game')
            at=len(g.log);g.shell('shikaku &');g.shell('game=$!');marker(f'SHIKAKU_SEED 10 {int(seed[0])} {int(seed[1])}',at);marker('SHIKAKU_PUZZLE 24 10 1',at);painted(at)
            # Large generation is cancellable without replacing the current board.
            toolbar(0);key(81);key(40);marker('SHIKAKU_PUZZLE 60 25 0',at)
            at=len(g.log);toolbar(1);marker('SHIKAKU_GENERATING 25 ',at);key(41);painted(at)
            assert b'SHIKAKU_SEED 25 ' not in g.log[at:]
            at=len(g.log);event(input=dict(type='close'));marker('SHIKAKU_EXIT 0',at);g.shell('wait $game')
            at=len(g.log);g.shell('shikaku 15 --seed 1 &');g.shell('game=$!');marker('SHIKAKU_SEED 15 1 ',at);painted(at)
            at=len(g.log);event(resize=[640,600]);marker('SHIKAKU_RESIZE 640 600',at);painted(at);capture('15-varied.ppm')
            at=len(g.log);key(20);marker('SHIKAKU_EXIT 0',at);g.shell('wait $game');g.shell('kill $server; wait $server')
            for bad in (b'GsError',b'NXR_BUFFER_FAILED',b'panic:',b'runtime error:',b'FAIL line'):assert bad not in g.log,g.log[-6000:]
            print('Shikaku/Fuse-X: rules, reverse drag, right/Ctrl/Shift erase, undo/redo, save/reopen, win, resize, zoom/fit/scroll, fifth grid lines, idle and close passed')
        finally:(output/'guest.log').write_bytes(g.log);g.close()

def ripple(output,check_rules=True):
    output.mkdir(parents=True,exist_ok=True);build=ROOT/'build/fuzix'
    with tempfile.TemporaryDirectory(prefix='shikaku-rvm-') as directory:
        work=Path(directory);apps={n:build/f'{n}.rxe' for n in ('nano-X','shikaku','shikakucheck','kill','klotskictl','nxcheck')}
        g=Guest(work,'ripple',apps,[dict(name='/dev/gfx',mode=0o20600,device=0x400)])
        try:
            if check_rules:
                result=g.shell('shikakucheck',seconds=240);assert 'failures=0' in result,result
        finally:g.close()
        subprocess.run(['cargo','test','--release','-p','rvm','--test','shikaku','--','--ignored','--nocapture'],cwd=ROOT,check=True,env=dict(os.environ,SHIKAKU_TEST_BUILD=str(build),SHIKAKU_TEST_OUTPUT=str(output),SHIKAKU_TEST_DISK=str(work/'disk.img')))
        assert 'NXCHECK_' not in (output/'guest.log').read_text()
        assert 'NXCHECK_' not in (output/'first-boot.log').read_text()
        subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(work/'disk.img')],check=True)
if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--target',choices=('native','ripple','both'),default='both');ap.add_argument('--gui-only',action='store_true');a=ap.parse_args()
    if a.target in ('native','both'):native(ROOT/'build/shikaku-tests/native',not a.gui_only)
    if a.target in ('ripple','both'):ripple(ROOT/'build/shikaku-tests/ripple',not a.gui_only)
