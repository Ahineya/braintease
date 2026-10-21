#!/usr/bin/env python3
"""Real Fuse-X input, pixels, resize, controls, persistence and concurrent games."""
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

def main():
    output=ROOT/'build/puzzles-tests/gui';output.mkdir(parents=True,exist_ok=True)
    build=ROOT/'fuse-x/build';current='mines'
    with tempfile.TemporaryDirectory(prefix='puzzles-gui-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'input.json'
        old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        try:g=Guest(work,'native',{n:build/'apps'/f'{n}.fxe' for n in ('nano-X','mines','solo','net','kill')},[dict(name='/dev/gfx',mode=0o20600,device=0x400)])
        finally:os.environ.clear();os.environ.update(old)
        def wait(test,seconds=120):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                g.pump();r=test()
                if r:return r
                if g.process.poll() is not None:break
            raise AssertionError(g.log[-6000:].decode(errors='replace'))
        def marker(s,at=0):return wait(lambda:s.encode() in g.log[at:])
        def window():
            def find():
                try:return next((w for w in json.loads((frames/'windows.json').read_text()) if w['visible'] and w['title'].startswith(current.capitalize()+' /')),None)
                except (FileNotFoundError,json.JSONDecodeError):return None
            return wait(find)
        def event(**fields):
            wid=window()['id'];wait(lambda:not mailbox.exists());temp=mailbox.with_suffix('.tmp');temp.write_text(json.dumps(dict(window=wid,**fields)));temp.replace(mailbox);wait(lambda:not mailbox.exists())
        def key(code):event(input=dict(type='key',key=code,down=True));event(input=dict(type='key',key=code,down=False))
        def click(x,y,button=0):
            event(input=dict(type='motion',x=x,y=y));event(input=dict(type='button',x=x,y=y,button=button,down=True));event(input=dict(type='button',x=x,y=y,button=button,down=False))
        def toolbar(i):click(8+i*((window()['width']-16)//8)+((window()['width']-16)//8)//2,56)
        def geometry():
            m=re.findall(rb'PUZZLES_LAYOUT '+current.encode()+rb' (\d+) (\d+) (\d+) (\d+) (\d+)',g.log);assert m
            return tuple(map(int,m[-1]))
        def settle():
            until=time.monotonic()+.3
            while time.monotonic()<until:g.pump();time.sleep(.01)
        def capture(name):
            settle()
            def get():
                try:
                    data=(frames/f"{window()['id']}.ppm").read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    return data if m and len(data)==m.end()+int(m[1])*int(m[2])*3 else None
                except FileNotFoundError:return None
            data=wait(get);(output/name).write_bytes(data);return data
        def board(data):
            header=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
            width=int(header[1]);pixels=data[header.end():]
            bx,by,pw,ph,_=geometry()
            return b''.join(pixels[((by+y)*width+bx)*3:((by+y)*width+bx+pw)*3] for y in range(ph))
        try:
            g.shell('PUZZLES_TRACE=1; export PUZZLES_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');marker('Nano-X ready')
            for current,params in [('mines','9x9n10'),('solo','3x3'),('net','5x5')]:
                at=len(g.log);g.shell(f'{current} {params}#123456789 &');g.shell('game=$!');marker(f'PUZZLES_READY {current} restored=0',at)
                assert window()['resizable'];initial=capture(f'{current}-initial.ppm');assert len(set(initial))>12
                if current=='net':
                    bx,by,pw,ph,tile=geometry();baseline=board(initial)
                    # Four turns, either direction, reproduce every board
                    # pixel. Locking must be visible and unlocking reversible.
                    for button in (0,1):
                        for turn in range(4):
                            click(bx+pw//2,by+ph//2,button)
                            rotated=capture(f'net-rotate-{button}-{turn}.ppm')
                        assert board(rotated)==baseline,'four Net rotations changed the board rendering'
                    click(bx+pw//2,by+ph//2,2)
                    assert board(capture('net-locked.ppm'))!=baseline,'Net lock is invisible'
                    click(bx+pw//2,by+ph//2,2)
                    assert board(capture('net-unlocked.ppm'))==baseline,'Net unlock changed the board rendering'
                bx,by,pw,ph,tile=geometry();at=len(g.log)
                if current=='mines':click(bx+pw//2,by+ph//2)
                elif current=='net':click(bx+pw//2,by+ph//2)
                else:
                    for y in range(3):
                        for x in range(3):
                            click(bx+tile//2+x*tile+tile//2,by+tile//2+y*tile+tile//2);key(30)
                            settle()
                            if f'PUZZLES_CHANGE {current} undo=1'.encode() in g.log[at:]:break
                        else:continue
                        break
                marker(f'PUZZLES_CHANGE {current} undo=1',at)
                assert capture(f'{current}-move.ppm')!=initial
                at=len(g.log);toolbar(2);marker(f'PUZZLES_CHANGE {current}',at)
                at=len(g.log);toolbar(3);marker(f'PUZZLES_CHANGE {current}',at)
                for w,h in [(900,740),(480,400),(700,600)]:
                    at=len(g.log);event(resize=[w,h]);marker(f'PUZZLES_RESIZE {current} {w} {h}',at);capture(f'{current}-{w}x{h}.ppm')
                # A modal preset menu handles navigation and exposes Custom.
                at=len(g.log);key(59);marker(f'PUZZLES_PAINT {current} 1 ',at);capture(f'{current}-types.ppm');key(41)
                at=len(g.log);key(60);marker(f'PUZZLES_PAINT {current} 4 ',at);capture(f'{current}-id.ppm');key(41)
                at=len(g.log);key(58);marker(f'PUZZLES_PAINT {current} 2 ',at);capture(f'{current}-help.ppm');key(41)
                # Save the actual move and reopen; full upstream history survives.
                settle();at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {current} 0',at);g.shell('wait $game')
                at=len(g.log);g.shell(f'{current} &');g.shell('game=$!');marker(f'PUZZLES_READY {current} restored=1',at);capture(f'{current}-restored.ppm')
                at=len(g.log);toolbar(2);marker(f'PUZZLES_CHANGE {current}',at)
                at=len(g.log);toolbar(3);marker(f'PUZZLES_CHANGE {current}',at)
                # Solve is explicit and undoable.
                toolbar(5);at=len(g.log);key(40);marker(f'PUZZLES_CHANGE {current} undo=1 redo=0 status={0 if current=="mines" else 1}',at);capture(f'{current}-solved.ppm')
                # New is endless; a fresh board resets completion.
                at=len(g.log);toolbar(0);marker(f'PUZZLES_CHANGE {current}',at);capture(f'{current}-new.ppm')
                previous=window()['title'];key(59);settle()
                at=len(g.log);click(40,82+36+(2 if current=='mines' else 0 if current=='solo' else 1)*27)
                marker(f'PUZZLES_CHANGE {current}',at);wait(lambda:window()['title']!=previous);capture(f'{current}-preset.ppm')
                key(59);key(77);at=len(g.log);key(40);marker(f'PUZZLES_PAINT {current} 3 ',at)
                capture(f'{current}-custom.ppm')
                # Reject zero dimensions and retain the existing game.
                event(input=dict(type='key',key=224,down=True));key(24);event(input=dict(type='key',key=224,down=False));key(39)
                at=len(g.log);key(40);marker(f'PUZZLES_PAINT {current} 3 ',at);capture(f'{current}-invalid.ppm');key(41)
                settle();sequence=window()['sequence']
                for _ in range(30):g.pump();time.sleep(.02)
                assert window()['sequence']==sequence,'idle repaint loop'
                at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {current} 0',at);g.shell('wait $game')
                print(current,'real moves, undo/redo, resize, menus, save/reopen, solve and new passed',flush=True)
            # The shared frontend is three independent clients of one server.
            for current in ('mines','solo','net'):
                at=len(g.log);g.shell(f'{current} &');g.shell(f'{current}pid=$!');marker(f'PUZZLES_READY {current} restored=1',at)
            def three():
                ws=json.loads((frames/'windows.json').read_text())
                return len([w for w in ws if w['visible'] and any(w['title'].startswith(n+' /') for n in ('Mines','Solo','Net'))])==3
            wait(three)
            (output/'windows.json').write_text((frames/'windows.json').read_text())
            for current in ('mines','solo','net'):capture(f'{current}-concurrent.ppm')
            for current in ('mines','solo','net'):
                at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {current} 0',at);g.shell(f'wait ${current}pid')
            g.shell('kill $server; wait $server')
            for bad in (b'GsError',b'panic:',b'runtime error:',b'assertion',b'NXR_BUFFER_FAILED'):assert bad not in g.log,g.log[-6000:]
            print('Three concurrent puzzle windows and clean shutdown passed',flush=True)
        finally:(output/'guest.log').write_bytes(g.log);g.close()

if __name__=='__main__':main()
