#!/usr/bin/env python3
"""Real Nano-X gameplay, menus, input, resize and saves on disposable disks."""
import argparse,json,os,re,shlex,shutil,subprocess,sys,time
from pathlib import Path
PORT=Path(__file__).resolve().parent;ROOT=PORT.parents[1]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest

def files():
    return [dict(name='/etc/passwd',mode=0o100644,text='root:x:0:0:root:/root:/bin/sh\n'),
            dict(name='/root/.nethackrc',mode=0o100600,text='OPTIONS=color,!news,!legacy,!autopickup,time,hilite_pet,menustyle:full,pettype:none\n'),
            dict(name='/etc/termcap',text='ansi|vt100|xterm|xterm-256color|ripple:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n'),
            dict(name='/dev/gfx',mode=0o20600,device=0x400)]

def native():
    work=PORT/'build/graphics-native';work.mkdir(parents=True,exist_ok=True)
    share=work/'share';share.mkdir(exist_ok=True)
    for name,source in [('nethack-nx.fxe','fuse-x-nx/nethack-nx.fxe'),('nethack.fxe','fuse-x/nethack.fxe')]:
        shutil.copy2(PORT/'build'/source,share/name)
    shutil.copytree(PORT/'build/nethack-data',share/'nethack-data',dirs_exist_ok=True)
    frames=work/'frames';mailbox=work/'input.json';mailbox.unlink(missing_ok=True)
    wrapper=work/'kernel-wrapper'
    wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(str(ROOT/'fuse-x/build/fuse-x'))+' "$@" --share '+shlex.quote(str(share))+'\n');wrapper.chmod(0o755)
    saved=dict(os.environ)
    os.environ.update(FUSE_X_FRAMES=str(frames),FUSE_X_TEST_EVENT=str(mailbox),FUSE_X_TEST_KERNEL=str(wrapper))
    try:g=Guest(work,'native',{n:ROOT/'fuse-x/build/apps'/(n+'.fxe') for n in ['nano-X','kill']},files(),blocks=65536)
    finally:os.environ.clear();os.environ.update(saved)
    wid=None;more_seen=0
    def wait(test,seconds=120):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            g.pump();value=test()
            if value:return value
            assert g.process.poll() is None,g.log[-5000:].decode(errors='replace')
            for bad in [b'panic:',b'GsError',b'Program initialization has failed',b'NXR_BUFFER_FAILED']:
                assert bad not in g.log,g.log[-5000:].decode(errors='replace')
        raise AssertionError(g.log[-5000:].decode(errors='replace'))
    def marker(text,at=0):
        def ready():
            nonlocal more_seen
            if wid is not None and b'NHNX_MORE' in g.log[more_seen:]:
                more_seen=len(g.log);key(44)
            return text.encode() in g.log[at:]
        return wait(ready)
    def registry():
        try:return json.loads((frames/'windows.json').read_text())
        except (FileNotFoundError,json.JSONDecodeError):return []
    def window():return wait(lambda:next((w for w in registry() if w['title']=='NetHack' and w['visible']),None))
    def event(**value):
        wait(lambda:not mailbox.exists());temporary=mailbox.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(window=wid,**value)));temporary.replace(mailbox);wait(lambda:not mailbox.exists())
    def key(code,shift=False,ctrl=False):
        for modifier,enabled in [(225,shift),(224,ctrl)]:
            if enabled:event(input=dict(type='key',key=modifier,down=True))
        event(input=dict(type='key',key=code,down=True));event(input=dict(type='key',key=code,down=False))
        for modifier,enabled in [(224,ctrl),(225,shift)]:
            if enabled:event(input=dict(type='key',key=modifier,down=False))
    def capture(name):
        p=frames/f'{wid}.ppm';wait(lambda:p.exists() and p.stat().st_size>10000)
        time.sleep(.2);data=p.read_bytes();(work/name).write_bytes(data);return data
    def turn(number,at):marker('NHNX_TURN '+str(number)+' ',at)
    def launch(restore=False,extra=' -p Valkyrie -r human -@'):
        nonlocal wid
        at=len(g.log);g.shell('/mnt/host/nethack-nx.fxe -u Tiles'+('' if restore else extra)+' &');g.shell('game=$!')
        marker('NHNX_READY',at);wid=window()['id'];turn(2 if restore else 1,at)
    def save():
        at=len(g.log);key(22,shift=True);marker('NHNX_PROMPT Really save',at);key(28)
        marker('NHNX_EXIT',at);g.shell('wait $game')
    report={}
    try:
        g.shell('NETHACK_TRACE=1; export NETHACK_TRACE; NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
        g.shell('nano-X -p &');g.shell('server=$!');marker('Nano-X ready')
        start=time.monotonic();launch();report['startup_seconds']=round(time.monotonic()-start,2)
        initial=capture('map.ppm');pixels=initial.split(b'\n',3)[3]
        assert len({pixels[i:i+3] for i in range(0,len(pixels),3)})>=12
        assert capture('idle.ppm')==initial,'Idle game redraw changed pixels'
        at=len(g.log);key(12);marker('NHNX_MENU',at);inventory=capture('inventory.ppm');assert inventory!=initial
        at=len(g.log);key(41);turn(1,at)
        at=len(g.log);key(58);marker('NHNX_MENU',at);assert capture('help.ppm')!=initial
        at=len(g.log);key(41);turn(1,at)
        at=len(g.log);key(7,ctrl=True);marker('NHNX_PROMPT In what direction',at)
        key(79);turn(2,at);capture('turn.ppm')
        at=len(g.log);key(19,ctrl=True);marker('Message history',at);capture('history.ppm')
        at=len(g.log);key(41);turn(2,at)
        # Multi-selection must toggle without leaving the menu; Escape cancels.
        at=len(g.log);key(7,shift=True);marker('NHNX_MENU',at);capture('drop.ppm')
        key(44);capture('drop-selected.ppm');at=len(g.log);key(41);turn(2,at)
        # Text input is visible even while an overlay is active.
        at=len(g.log);key(32,shift=True);marker('NHNX_PROMPT #',at);capture('command.ppm')
        for letter in 'conduct':key(ord(letter)-ord('a')+4)
        capture('command-typed.ppm');at=len(g.log);key(40);marker('NHNX_MENU',at);capture('conduct.ppm')
        at=len(g.log);key(41);turn(2,at)
        for width,height in [(1000,650),(480,360),(800,550)]:
            event(resize=[width,height]);wait(lambda:window()['width']==width and window()['height']==height)
            data=capture(f'resize-{width}.ppm');assert data.startswith(f'P6\n{width} {height}\n255\n'.encode())
        # A close request must reach the ordinary save confirmation.
        at=len(g.log);event(input=dict(type='close'));marker('NHNX_PROMPT Really save',at)
        key(28);marker('NHNX_EXIT',at);g.shell('wait $game')
        launch(True);capture('restored.ppm');save()
        # The tty and graphical variants must agree on the save format.
        os.write(g.master,b'/mnt/host/nethack.fxe -u Tiles\r')
        g.read(b'Restoring save file');
        until=time.monotonic()+60;more_at=g.at
        while b'T:2' not in g.log[g.at:]:
            g.pump()
            if b'--More--' in g.log[more_at:]:more_at=len(g.log);os.write(g.master,b' ')
            assert time.monotonic()<until,g.log[-5000:]
        os.write(g.master,b'S');g.read(b'Really save');os.write(g.master,b'y');g.read(g.prompt)
        launch(True);capture('from-tty.ppm');save()
        # Exercise all graphical character-selection facets, rather than only
        # the fully specified command-line shortcut used by the gameplay test.
        at=len(g.log);g.shell('/mnt/host/nethack-nx.fxe -u Choose &');g.shell('game=$!')
        marker('NHNX_READY',at);wid=window()['id'];marker('NHNX_PROMPT Shall I pick',at)
        at=len(g.log);key(17);marker('Choose your role',at);capture('character-role.ppm')
        key(81)
        for facet in ['race','gender','alignment']:
            at=len(g.log);key(40);marker('Choose your '+facet,at)
        at=len(g.log);key(40);marker('NHNX_PROMPT Play a ',at);capture('character-confirm.ppm')
        at=len(g.log);key(28);turn(1,at);save()
        g.shell('kill $server');g.shell('wait $server')
        report['status']='passed'
    finally:
        (work/'guest.log').write_bytes(g.log);g.close()
    subprocess.run(['node',str(PORT/'verify_state.mjs'),str(work/'disk.img'),'native','/root','Tiles'],check=True,cwd=ROOT)
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

def ripple():
    work=PORT/'build/graphics-ripple';work.mkdir(parents=True,exist_ok=True);build=ROOT/'build/fuzix'
    manifest=files()+[dict(name='/bin/nethack-nx',path=str(PORT/'build/fuzix-nx/nethack-nx.rxe'),mode=0o100755)]
    manifest += [dict(name='/bin/'+name,path=str(build/(name+'.rxe')),mode=0o100755) for name in ['nano-X','kill']]
    manifest += [dict(name='/usr/share/nethack/'+p.name,path=str(p)) for p in (PORT/'build/nethack-data').iterdir()]
    (work/'manifest.json').write_text(json.dumps(manifest))
    subprocess.run(['node',str(PORT/'test_image.mjs'),str(work/'manifest.json'),str(work/'disk.img'),'ripple',str(build)],check=True,cwd=ROOT)
    subprocess.run(['cargo','test','--release','-p','rvm','--test','nethack','--','--ignored','--nocapture'],check=True,cwd=ROOT,
        env=dict(os.environ,NETHACK_TEST_BUILD=str(build),NETHACK_TEST_OUTPUT=str(work),NETHACK_TEST_DISK=str(work/'disk.img')))
    subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(work/'disk.img')],check=True,cwd=ROOT)
    subprocess.run(['node',str(PORT/'verify_state.mjs'),str(work/'disk.img'),'ripple','/root','Tiles'],check=True,cwd=ROOT)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('target',choices=['native','ripple']);a=p.parse_args()
    native() if a.target=='native' else ripple()
