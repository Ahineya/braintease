#!/usr/bin/env python3
"""Launch every puzzle through the real GUI; exercise drawing, resize and saves."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
GAMES=json.loads((ROOT/'ports/puzzles/catalog.json').read_text())
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--only',nargs='+',choices=[g['id'] for g in GAMES])
    ap.add_argument('--output',type=Path,default=ROOT/'build/puzzles-tests/collection-gui');a=ap.parse_args()
    output=a.output;output.mkdir(parents=True,exist_ok=True)
    build=ROOT/'fuse-x/build';selected=[game for game in GAMES if not a.only or game['id'] in a.only]
    with tempfile.TemporaryDirectory(prefix='puzzles-collection-gui-') as directory:
        work=Path(directory);frames=work/'frames';mailbox=work/'input.json'
        old=dict(os.environ);os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox))
        files=[dict(name='/dev/gfx',mode=0o20600,device=0x400),
               dict(name='/tmp/games/puzzles.fxe',mode=0o100755,path=str(build/'apps/puzzles.fxe')),
               dict(name=f'/tmp/games/puzzles/{selected[0]["id"]}.fxe',mode=0o100755,path=str(build/'apps'/f'{selected[0]["id"]}.fxe'))]
        try:g=Guest(work,'native',{n:build/'apps'/f'{n}.fxe' for n in ['nano-X','puzzles','kill',*[game['id'] for game in selected]]},files,blocks=262144)
        finally:os.environ.clear();os.environ.update(old)
        title='Portable Puzzles'
        def wait(test,seconds=120):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                g.pump();result=test()
                if result:return result
                if g.process.poll() is not None:break
            raise AssertionError(g.log[-6000:].decode(errors='replace'))
        def marker(s,at=0):return wait(lambda:s.encode() in g.log[at:])
        def window():
            def find():
                try:return next((w for w in json.loads((frames/'windows.json').read_text()) if w['visible'] and (w['title']==title or w['title'].startswith(title+' /'))),None)
                except (FileNotFoundError,json.JSONDecodeError):return None
            return wait(find)
        def event(**fields):
            wid=window()['id'];wait(lambda:not mailbox.exists());tmp=mailbox.with_suffix('.tmp');tmp.write_text(json.dumps(dict(window=wid,**fields)));tmp.replace(mailbox);wait(lambda:not mailbox.exists())
        def key(code):event(input=dict(type='key',key=code,down=True));event(input=dict(type='key',key=code,down=False))
        def click(x,y,button=0):
            event(input=dict(type='motion',x=x,y=y));event(input=dict(type='button',x=x,y=y,button=button,down=True));event(input=dict(type='button',x=x,y=y,button=button,down=False))
        def settle():
            end=time.monotonic()+.25
            while time.monotonic()<end:g.pump();time.sleep(.01)
        def capture(name):
            settle()
            def get():
                try:
                    data=(frames/f"{window()['id']}.ppm").read_bytes();m=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    return data if m and len(data)==m.end()+int(m[1])*int(m[2])*3 else None
                except FileNotFoundError:return None
            data=wait(get);(output/name).write_bytes(data);return data
        try:
            g.shell('PUZZLES_TRACE=1; export PUZZLES_TRACE');g.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            g.shell('nano-X -p &');g.shell('server=$!');marker('Nano-X ready')
            g.shell('puzzles &');g.shell('launcher=$!');marker('PUZZLES_LAUNCHER_READY 40');capture('launcher.ppm')
            # Launcher navigation, resizing, search and child cleanup all use
            # the same paths a player uses. A new selection resets scrolling.
            event(resize=[900,740]);settle();capture('launcher-large.ppm')
            event(resize=[640,480]);settle()
            for game in selected:
                name=game['id'];title='Portable Puzzles'
                event(input=dict(type='key',key=224,down=True));key(24);event(input=dict(type='key',key=224,down=False))
                for ch in name:key(ord(ch)-ord('a')+4)
                settle();at=len(g.log);key(40);title=game.get('window_name',game['name']);marker(f'PUZZLES_READY {name} restored=0',at)
                assert window()['resizable'];initial=capture(f'{name}-initial.ppm');assert len(set(initial))>12
                event(resize=[800,640]);marker(f'PUZZLES_RESIZE {name} 800 640',at);capture(f'{name}-large.ppm')
                event(resize=[480,400]);marker(f'PUZZLES_RESIZE {name} 480 400',at);capture(f'{name}-small.ppm')
                # Real mouse/keyboard input, plus common dialogs.
                matches=re.findall(rb'PUZZLES_LAYOUT '+name.encode()+rb' (\d+) (\d+) (\d+) (\d+) (\d+)',g.log)
                bx,by,pw,ph,tile=map(int,matches[-1])
                for part in (1,2,3):
                    click(bx+pw*part//4,by+ph*part//4);key(30+part-1)
                key(79);key(81);settle();capture(f'{name}-moves.ppm')
                key(58);settle();capture(f'{name}-help.ppm');key(41)
                key(59);settle();capture(f'{name}-types.ppm');key(41)
                at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {name} 0',at)
                # A child exit must not leave a stale "already open" marker.
                title='Portable Puzzles';settle();at=len(g.log);key(40);title=game.get('window_name',game['name']);marker(f'PUZZLES_READY {name} restored=1',at)
                capture(f'{name}-restored.ppm');at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {name} 0',at)
                print(name,'launcher, pixels, resize, input, help, presets, save/reopen passed',flush=True)
            title='Portable Puzzles';settle();at=len(g.log)
            # Missing executable is a visible error, not a launcher exit.
            if a.only:
                event(input=dict(type='key',key=224,down=True));key(24);event(input=dict(type='key',key=224,down=False))
                for ch in next(game['id'] for game in GAMES if game not in selected):key(ord(ch)-ord('a')+4)
                key(40);capture('launcher-missing.ppm');assert window()['visible']
            event(input=dict(type='close'));g.shell('wait $launcher')
            # The same launcher discovers sibling puzzles/*.fxe outside /bin.
            at=len(g.log);g.shell('/tmp/games/puzzles.fxe &');g.shell('launcher=$!');marker('PUZZLES_LAUNCHER_READY 40',at)
            for ch in selected[0]['id']:key(ord(ch)-ord('a')+4)
            at=len(g.log);key(40);title=selected[0].get('window_name',selected[0]['name']);marker(f'PUZZLES_READY {selected[0]["id"]} restored=1',at)
            at=len(g.log);event(input=dict(type='close'));marker(f'PUZZLES_EXIT {selected[0]["id"]} 0',at)
            title='Portable Puzzles';settle();event(input=dict(type='close'));g.shell('wait $launcher');g.shell('kill $server; wait $server')
            for bad in (b'GsError',b'panic:',b'runtime error:',b'assertion',b'NXR_BUFFER_FAILED'):assert bad not in g.log,g.log[-6000:]
            print('Collection launcher and all selected games shut down cleanly',flush=True)
        finally:(output/'guest.log').write_bytes(g.log);g.close()


if __name__=='__main__':main()
