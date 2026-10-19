#!/usr/bin/env python3
"""Klotski input, pixels, resize and persistence on disposable guest disks."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def fixtures(work):
    trace=json.loads((ROOT/'ports/klotski/tests/solution.json').read_text())
    steps=trace['steps'][:-1];journal=f"KLOTSKI 1 {trace['signature']} {len(steps)} {len(steps)} 0\n"
    journal+=''.join(f'{ord(c)} {dx} {dy}\n' for c,dx,dy in steps)
    file=work/'almost-solved';file.write_text(journal)
    return [dict(name='/dev/gfx',mode=0o20600,device=0x400),
            dict(name='/wins/level-01',path=str(file),mode=0o100600)]


def native(output):
    output.mkdir(parents=True,exist_ok=True)
    build=ROOT/'fuse-x/build'
    with tempfile.TemporaryDirectory(prefix='klotski-native-') as folder:
        work=Path(folder);frames=work/'frames';mailbox=work/'input.json'
        env=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        apps={n:build/'apps'/f'{n}.fxe' for n in ('nano-X','klotski','klotskicheck','kill')}
        extra=fixtures(work)
        try:g=Guest(work,'native',apps,extra)
        finally:os.environ.clear();os.environ.update(env)
        def wait(test,seconds=90):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                g.pump();v=test()
                if v:return v
                assert g.process.poll() is None,g.log[-6000:].decode(errors='replace')
            raise AssertionError(g.log[-6000:].decode(errors='replace'))
        def window():
            def find():
                try:return next((w for w in json.loads((frames/'windows.json').read_text()) if w['visible'] and w['title'].startswith('Klotski /')),None)
                except (FileNotFoundError,json.JSONDecodeError):return None
            return wait(find)
        def event(**values):
            wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp')
            tmp.write_text(json.dumps(dict(window=window()['id'],**values)));tmp.replace(mailbox);wait(lambda:not mailbox.exists())
        def key(code):
            event(input=dict(type='key',key=code,down=True));event(input=dict(type='key',key=code,down=False))
        def marker(text,start):wait(lambda:text.encode() in g.log[start:])
        def capture(name,size=None):
            def frame():
                try:
                    data=(frames/f"{window()['id']}.ppm").read_bytes()
                    m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    if not m:return None
                    w,h=map(int,m.groups())
                    return data if len(data)==m.end()+w*h*3 and (size is None or (w,h)==size) else None
                except FileNotFoundError:return None
            data=wait(frame);(output/name).write_bytes(data);return data
        try:
            result=g.shell('klotskicheck',seconds=180);assert 'failures=0' in result,result
            assert 'Sunshine' in g.shell('klotski --list')
            g.shell('KLOTSKI_TRACE=1; export KLOTSKI_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');wait(lambda:b'Nano-X ready' in g.log)
            at=len(g.log);g.shell('klotski 1 &');g.shell('game=$!');marker('KLOTSKI_READY',at)
            ready=wait(lambda:next(iter(re.findall(rb'KLOTSKI_READY (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\r?\n',g.log)),None))
            _,w,h,tile,bx,by=map(int,ready)
            assert window()['resizable'] is True
            initial=capture('initial.ppm',(w,h));assert len(set(initial))>40
            # A coalesced two-cell drag of the bottom-left piece, with actual
            # pointer/button events, must remain one grouped move.
            x=bx+tile+tile//2;y=by+5*tile+tile//2
            event(input=dict(type='motion',x=x,y=y));event(input=dict(type='button',button=0,down=True,x=x,y=y))
            at=len(g.log);event(input=dict(type='motion',x=x+2*tile,y=y))
            event(input=dict(type='button',button=0,down=False,x=x+2*tile,y=y));marker('KLOTSKI_MOVE 1 1 2 0',at)
            time.sleep(.15);moved=capture('dragged.ppm');assert moved!=initial
            at=len(g.log);key(80);key(80);marker('KLOTSKI_MOVE 1 0 4 0',at)
            at=len(g.log);key(24);marker('KLOTSKI_MOVE 1 1 3 0',at) # U
            at=len(g.log);key(28);marker('KLOTSKI_MOVE 1 0 4 0',at) # Y
            at=len(g.log);key(24);marker('KLOTSKI_MOVE 1 1 3 0',at)
            for ww,hh in ((900,650),(480,340),(600,440)):
                at=len(g.log);event(resize=[ww,hh]);marker(f'KLOTSKI_RESIZE {ww} {hh}',at)
                time.sleep(.15);capture(f'resize-{ww}x{hh}.ppm',(ww,hh))
            key(58);time.sleep(.15);capture('help.ppm');key(41)
            at=len(g.log);key(20);marker('KLOTSKI_EXIT 0',at);assert '0' in g.shell('wait $game; echo EXIT:$?')
            at=len(g.log);g.shell('klotski &');g.shell('game=$!');marker('KLOTSKI_READY',at)
            marker('KLOTSKI_LEVEL 1 1 3',at)
            # Redo history also survives process restart.
            at=len(g.log);key(28);marker('KLOTSKI_MOVE 1 0 4 0',at)
            key(6);key(79);key(79) # C; Challenge; Skill
            for _ in range(8):key(81)
            at=len(g.log);key(40);marker('KLOTSKI_LEVEL 28 0 0',at)
            time.sleep(.2);capture('sunshine.ppm')
            # No timer/animation or idle repaint traffic.
            time.sleep(.3);sequence=window()['sequence'];stable=capture('idle.ppm')
            for _ in range(10):g.pump();time.sleep(.03)
            assert window()['sequence']==sequence
            assert capture('idle-again.ppm')==stable
            # Restart requires confirmation; Escape retains the current game.
            key(21);time.sleep(.1);capture('restart-dialog.ppm');key(41)
            at=len(g.log);event(input=dict(type='close'));marker('KLOTSKI_EXIT 0',at);g.shell('wait $game')
            # Complete the original first puzzle from a valid, replay-checked
            # independently generated trace. No test-only game rules or hooks.
            at=len(g.log);g.shell('KLOTSKI_HOME=/wins klotski 1 &');g.shell('game=$!');marker('KLOTSKI_READY',at)
            ready=wait(lambda:next(iter(re.findall(rb'KLOTSKI_READY (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\r?\n',g.log[at:])),None))
            _,w,h,tile,bx,by=map(int,ready)
            trace=json.loads((ROOT/'ports/klotski/tests/solution.json').read_text())
            gx,gy=trace['goal_before_final'];x=bx+gx*tile+tile//2;y=by+gy*tile+tile//2
            event(input=dict(type='motion',x=x,y=y));event(input=dict(type='button',button=0,down=True,x=x,y=y))
            event(input=dict(type='button',button=0,down=False,x=x,y=y))
            at=len(g.log);key(79);marker(f"KLOTSKI_MOVE 1 {trace['score']} 32 1",at)
            marker(f"KLOTSKI_SAVE 1 32 32 {trace['score']}",at);time.sleep(.15);capture('solved.ppm')
            at=len(g.log);event(input=dict(type='close'));marker('KLOTSKI_EXIT 0',at);g.shell('wait $game')
            g.shell('kill $server; wait $server')
            for bad in (b'GsError',b'NXR_BUFFER_FAILED',b'panic:',b'FAIL line',b'runtime error:'):assert bad not in g.log,g.log.decode(errors='replace')
            (output/'first-boot.log').write_bytes(g.log)
            g.close()
            previous=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
            try:g=Guest(work,'native',apps,extra,reuse=True)
            finally:os.environ.clear();os.environ.update(previous)
            g.shell('KLOTSKI_TRACE=1; export KLOTSKI_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');wait(lambda:b'Nano-X ready' in g.log)
            at=len(g.log);g.shell('KLOTSKI_HOME=/wins klotski 1 &');g.shell('game=$!');marker('KLOTSKI_READY',at)
            marker(f"KLOTSKI_LEVEL 1 {trace['score']} 32",at);time.sleep(.15);capture('solved-after-reboot.ppm')
            at=len(g.log);key(21);key(40);marker(f"KLOTSKI_SAVE 1 0 0 {trace['score']}",at)
            time.sleep(.15);capture('restarted-best-kept.ppm')
            at=len(g.log);event(input=dict(type='close'));marker('KLOTSKI_EXIT 0',at);g.shell('wait $game')
            g.shell('kill $server; wait $server')
            print('Klotski/Fuse-X: rules, dragging/keys, undo/redo, resize, largest puzzle, win, scores across reboot, restart, idle and cleanup passed')
        finally:
            (output/'guest.log').write_bytes(g.log);g.close()


def ripple(output,test_binary=None):
    output.mkdir(parents=True,exist_ok=True);build=ROOT/'build/fuzix'
    with tempfile.TemporaryDirectory(prefix='klotski-rvm-') as folder:
        work=Path(folder)
        # Guest creates the same disposable image/SDK layout used by ordinary
        # guest probes. Shut down that first boot before the in-process JIT test.
        apps={n:build/f'{n}.rxe' for n in ('nano-X','klotski','klotskicheck','klotskictl','kill')}
        g=Guest(work,'ripple',apps,fixtures(work))
        try:
            result=g.shell('klotskicheck',seconds=180);assert 'failures=0' in result,result
        finally:g.close()
        command=[str(Path(test_binary).resolve()),'--ignored','--nocapture'] if test_binary else ['cargo','test','--release','-p','rvm','--test','klotski','--','--ignored','--nocapture']
        subprocess.run(command,cwd=ROOT,check=True,env=dict(os.environ,KLOTSKI_TEST_BUILD=str(build),KLOTSKI_TEST_OUTPUT=str(output),KLOTSKI_TEST_DISK=str(work/'disk.img')))
        subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(work/'disk.img')],check=True)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=('native','ripple','both'),default='both');p.add_argument('--rvm-test-binary');a=p.parse_args()
    if a.target in ('native','both'):native(ROOT/'build/klotski-tests/native')
    if a.target in ('ripple','both'):ripple(ROOT/'build/klotski-tests/ripple',a.rvm_test_binary)
