#!/usr/bin/env python3
"""Exercise the real guest calculator on a fresh offline explicit-manifest disk."""
import json
import hashlib
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

def ppm(path):
    data=path.read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
    if not m:return None
    w,h=map(int,m.groups());pixels=data[m.end():]
    return (w,h,pixels) if len(pixels)==w*h*3 else None

def main():
    build=ROOT/'build/lvgl';scale=2 if '--retina' in sys.argv else 1
    output=build/('tests-2x' if scale==2 else 'tests');output.mkdir(exist_ok=True)
    (output/'result.json').unlink(missing_ok=True)
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(PORT/'model-test.c'),str(PORT/'calc.c'),
                    '--static','--float-format','-o',str(build/'model-test.fxe')],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='lvcalc-guest-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'input.json'
        saved=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{'lvcalc':build/'lvcalc.fxe','calc-model':build/'model-test.fxe'},
                          [dict(name='/dev/gfx',mode=0o20600,device=0x400)],blocks=32768)
        finally:os.environ.clear();os.environ.update(saved)
        def wait(test,seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                g.pump();value=test()
                if value:return value
                assert g.process.poll() is None,g.log[-6000:].decode(errors='replace')
            raise AssertionError('calculator wait timed out:\n'+g.log[-6000:].decode(errors='replace'))
        def registry():
            try:return json.loads((frames/'windows.json').read_text())
            except (FileNotFoundError,json.JSONDecodeError):return []
        def windows():return [w for w in registry() if w['visible'] and w['title']=='Calculator']
        def event(wid,**values):
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp');tmp.write_text(json.dumps(dict(window=wid,**values)));tmp.replace(mailbox)
            wait(lambda:not mailbox.exists())
        def key(wid,code):
            event(wid,input=dict(type='key',key=code,down=True));event(wid,input=dict(type='key',key=code,down=False))
        def text(wid,s):
            for c in s:event(wid,input=dict(type='text',scalar=ord(c)))
        def click(wid,x,y):
            event(wid,input=dict(type='button',button=0,down=True,x=x,y=y))
            event(wid,input=dict(type='button',button=0,down=False,x=x,y=y))
        def frame(wid,w=300,h=360):
            def read():
                try:
                    value=ppm(frames/f'{wid}.ppm')
                    return value if value and value[:2]==(w*scale,h*scale) and value[2][:3]==bytes((25,26,28)) else None
                except FileNotFoundError:return None
            return wait(read)
        def calculate(wid,expression,expected):
            text(wid,'c');text(wid,expression);at=len(g.log);key(wid,40)
            wait(lambda:('CALC_RESULT '+expected+'\r\n').encode() in g.log[at:] or ('CALC_RESULT '+expected+'\n').encode() in g.log[at:])
        try:
            assert 'CALC_MODEL passed' in g.shell('calc-model')
            g.shell(f'LVGL_SCALE={scale} lvcalc --trace &');g.shell('calculator=$!');wait(lambda:len(windows())==1);wid=windows()[0]['id']
            actual=frame(wid);shutil.copyfile(frames/f'{wid}.ppm',output/'initial.ppm')
            reference=ppm(build/('host/reference-2x.ppm' if scale==2 else 'host/reference.ppm'));assert actual[:2]==reference[:2]
            assert windows()[0]['format']==2,'Calculator did not negotiate full colour'
            equal=sum(actual[2][i:i+3]==reference[2][i:i+3] for i in range(0,len(actual[2]),3))
            total=len(actual[2])//3
            (output/'pixels.json').write_text(json.dumps(dict(equal=equal,total=total,fraction=equal/total),indent=2)+'\n')
            assert equal==total,f'Guest/reference pixel mismatch: {equal}/{total}'
            calculate(wid,'2+3*4','14');calculate(wid,'(2+3)*4','20');calculate(wid,'-3.5/2','-1.75')
            text(wid,'c1/0');at=len(g.log);key(wid,40);wait(lambda:b'CALC_ERROR Cannot divide by zero' in g.log[at:])
            # Click stock widgets: C, 7, +, 3, =.
            click(wid,43,144);click(wid,43,193);click(wid,256,291);click(wid,185,291)
            at=len(g.log);click(wid,256,340);wait(lambda:b'CALC_RESULT 10' in g.log[at:])
            # Native clipboard actions; select all replaces the expression.
            event(wid,action=4);event(wid,action=2,clipboard='(8+2)/4');at=len(g.log);key(wid,40)
            wait(lambda:b'CALC_RESULT 2.5' in g.log[at:]);event(wid,action=1);event(wid,action=3)
            event(wid,action=2);at=len(g.log);key(wid,40);wait(lambda:b'CALC_RESULT 2.5' in g.log[at:])
            # A physical key with committed text must insert once, not twice.
            text(wid,'c');event(wid,input=dict(type='text_key',key=31,down=True));text(wid,'2')
            event(wid,input=dict(type='text_key',key=31,down=False));text(wid,'+3');at=len(g.log);key(wid,40)
            wait(lambda:b'CALC_RESULT 5' in g.log[at:])
            # Navigation + delete uses LVGL's stock text entry.
            text(wid,'c12');key(wid,80);key(wid,42);at=len(g.log);key(wid,40);wait(lambda:b'CALC_RESULT 2\r\n' in g.log[at:])
            # Stock group navigation activates '(' and '=' by keyboard.
            text(wid,'c');key(wid,43);key(wid,43);key(wid,40);text(wid,'2+3)')
            at=len(g.log);key(wid,40);wait(lambda:b'CALC_RESULT 5\r\n' in g.log[at:])
            event(wid,input=dict(type='key',key=225,down=True));key(wid,43)
            event(wid,input=dict(type='key',key=225,down=False));at=len(g.log);key(wid,40)
            wait(lambda:b'CALC_RESULT 5\r\n' in g.log[at:])
            # Focus loss cancels an outstanding pointer press, not a click.
            text(wid,'c');at=len(g.log);event(wid,input=dict(type='button',button=0,down=True,x=43,y=193))
            event(wid,input=dict(type='focus',focused=False));event(wid,input=dict(type='focus',focused=True))
            until=time.monotonic()+.1
            while time.monotonic()<until:g.pump()
            assert b'CALC_INPUT' not in g.log[at:],'Focus loss activated a pressed button'
            # Native clipboard rejects non-numeric text without changing entry.
            text(wid,'12');event(wid,action=2,clipboard='hello');at=len(g.log);key(wid,40)
            wait(lambda:b'CALC_RESULT 12\r\n' in g.log[at:])
            text(wid,'c')
            for code in (89,90,87,91):key(wid,code)
            at=len(g.log);key(wid,88);wait(lambda:b'CALC_RESULT 15\r\n' in g.log[at:])
            for w,h,accepted in [(700,600,(700,600)),(32,32,(260,320)),(1600,1080,(1600,1080)),(300,360,(300,360))]:
                event(wid,resize=[w,h]);frame(wid,*accepted)
            calculate(wid,'6*7','42');shutil.copyfile(frames/f'{wid}.ppm',output/'result.ppm')
            # No caret blink/animations: a stable scene must stop publishing.
            # Console results precede the next refresh; allow that frame to
            # complete before measuring an idle interval spanning blink timers.
            until=time.monotonic()+.2
            while time.monotonic()<until:g.pump()
            path=frames/f'{wid}.ppm';before=path.stat().st_mtime_ns
            shutil.copyfile(path,output/'result.ppm')
            until=time.monotonic()+1.3
            while time.monotonic()<until:g.pump()
            shutil.copyfile(path,output/'idle-after.ppm')
            assert path.stat().st_mtime_ns==before,'Idle calculator keeps publishing frames'
            # Per-process windows and clean owner teardown.
            g.shell(f'LVGL_SCALE={scale} lvcalc &');g.shell('second=$!');wait(lambda:len(windows())==2)
            second=next(w['id'] for w in windows() if w['id']!=wid);frame(second)
            event(second,close=True);assert 'SECOND_STATUS:0' in g.shell('wait $second; echo SECOND_STATUS:$?')
            assert any(w['id']==wid for w in windows());calculate(wid,'9+1','10')
            event(wid,close=True);assert 'CALC_STATUS:0' in g.shell('wait $calculator; echo CALC_STATUS:$?');wait(lambda:not windows())
            assert not any(e in g.log for e in (b'native runtime error',b'panic:',b'Assertion `',b'graphics operation failed'))
            report=dict(status='passed',scale=scale,format='XRGB8888',pixels_equal=equal,pixels_total=total,
                executable_sha256=hashlib.sha256((build/'lvcalc.fxe').read_bytes()).hexdigest(),
                checks=['guest arithmetic','exact host/guest full-colour pixels','mouse buttons','keyboard editing','Tab/Shift-Tab/Enter','focus cancellation','numeric keypad','native clipboard',
                        'committed text without duplicate','resize bounds','idle damage','two instances','close cleanup'])
        finally:
            try:g.close()
            finally:(output/'guest.log').write_bytes(g.log)
        (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('LVGL/Fuse-X calculator: guest rendering, arithmetic, input, clipboard, resize, idle and lifecycle passed')

if __name__=='__main__':main()
