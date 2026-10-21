#!/usr/bin/env python3
"""Exercise mux on real kernels, private disks and a real outer terminal."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
sys.path.insert(0,str(ROOT/'rfl/tools'))
from test_os import Guest, run


def image(system,build,directory):
    names=['mux','muxcheck','ptysh','sh','cat','echo','sleep','stty','ps','seq','levee','rift','rfl-fm','lynx']
    stage=directory/'stage';stage.mkdir()
    if system=='fuzix':
        for name in names+['init']:(stage/(name+'.rxe')).symlink_to(build/(name+'.rxe'))
        for name in ['FUZIX.SYS','bios.bin']:(stage/name).symlink_to(build/name)
        (stage/'apps.json').write_text(json.dumps(names+['init']))
        disk=directory/'disk.img';run(['node',PORT/'tools/disk.mjs',stage,disk]);return disk
    for name in names:(stage/(name+'.fxe')).symlink_to(build/'apps'/(name+'.fxe'))
    (stage/'native-hello.fxe').symlink_to(build/'apps/native-hello.fxe')
    (directory/'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
    disk=directory/'disk.img';run(['node',ROOT/'fuse-x/image.mjs','create',disk,'--apps',stage]);return disk


def exercise(args,system,directory):
    build=(args.native_build if system=='fuse-x' else args.build).resolve()
    disk=image(system,build,directory)
    command=[build/'fuse-x','--run',disk] if system=='fuse-x' else [ROOT/'target/release/rvm',build/'bios.bin','--disk',disk,'--memory','16384000','--net','off','--jit','--guest-ctrl-c','--timeout','600']
    g=Guest(command,system)
    def visible(text):
        print(system+': waiting for '+text,flush=True)
        g.visible(text)
    prefix=b'\x02'
    def key(key):g.send(prefix+key)
    def line(text):g.send(text.encode()+b'\r')
    def settle():
        # Rendered-state checks below remain the synchronization authority.
        end=time.monotonic()+.2
        while time.monotonic()<end:g.pump()
    def pane_command(command,token):
        line(command);visible(token);settle()
    try:
        g.raw(g.prompt)
        capacity=g.shell('muxcheck')
        assert b'MUXCHECK_OK 16 PTYs' in capacity,capacity
        line('mux');visible('1:shell 1');visible('^B');settle()
        pane_command('echo FIRST_PANE', 'FIRST_PANE')
        key(b'c');visible('2:shell 2');settle()
        pane_command('echo SECOND_PANE', 'SECOND_PANE')
        key(b'1');g.wait(lambda:'FIRST_PANE' in g.screen.text() and 'SECOND_PANE' not in g.screen.text(),'tab isolation')
        key(b'2');line('seq 1 2000');key(b'1')
        pane_command('echo FOREGROUND_FAIR','FOREGROUND_FAIR')
        key(b'2');visible('2000');key(b'1');visible('FIRST_PANE')
        key(b',');visible('Rename tab:');g.send(b'\x15work\r')
        # Ctrl-U in prompts clears the existing name.
        visible('1:work')
        key(b'%');settle();pane_command('echo RIGHT_PANE', 'RIGHT_PANE');visible('FIRST_PANE')
        # Outer terminal mouse coordinates focus a pane and drag a divider.
        g.send(b'\x1b[<0;3;3M\x1b[<0;3;3m')
        pane_command('echo MOUSE_LEFT', 'MOUSE_LEFT')
        g.send(b'\x1b[<0;60;3M\x1b[<0;60;3m')
        pane_command('muxcheck --size', 'MUX_GEOMETRY 40 22')
        g.send(b'\x1b[<0;40;3M\x1b[<32;50;3M\x1b[<0;50;3m');settle()
        pane_command('muxcheck --size', 'MUX_GEOMETRY 30 22')
        g.send(b'\x1b[<0;50;3M\x1b[<32;40;3M\x1b[<0;40;3m');settle()
        key(b'"');settle();pane_command('echo LOWER_PANE', 'LOWER_PANE');visible('RIGHT_PANE')
        key(b'z');visible('ZOOM');g.wait(lambda:'RIGHT_PANE' not in g.screen.text(),'zoom hides sibling panes')
        key(b'z');visible('RIGHT_PANE')
        key(b'h');pane_command('echo LEFT_FOCUS', 'LEFT_FOCUS')
        key(b'+');settle();key(b'-');settle()
        key(b'?');visible('persistent sessions');g.send(b'q');visible('LEFT_FOCUS')
        # Resize propagates to the foreground process. A tiny window temporarily
        # presents only the focused pane and restores the split tree afterward.
        key(b'z');line('muxcheck --resize');visible('MUX_RESIZE_READY')
        g.resize(40,120);visible('MUX_GEOMETRY 120 38')
        g.resize(8,20);visible('MUX_GEOMETRY 20 6')
        g.resize(24,80);visible('MUX_GEOMETRY 80 22')
        g.send(b'\x03');settle();pane_command('echo INTERRUPTED_OK','INTERRUPTED_OK')
        line('sleep 30');settle();g.send(b'\x1a');visible('Stopped')
        line('fg');settle();g.send(b'\x03');settle();pane_command('echo JOB_CONTROL_OK','JOB_CONTROL_OK')
        line('muxcheck --keys');visible('MUX_KEYS_READY')
        g.send(b'A\t\x03\x1a\x02\x02\x00');visible('MUX_KEYS_DONE')
        for n in [65,9,3,26,2,0]:visible('MUX_KEY '+str(n))
        line('muxcheck --burst');visible('MUX_BURST_READY')
        for _ in range(24):g.send(b'x'*250)
        visible('MUX_BURST_OK 6000')
        # Levee receives Backspace and shell job control inside the pane.
        line('levee /tmp/mux-note');settle();g.send(b'iHELLOx\x7f\x1b:wq\r');settle()
        pane_command('cat /tmp/mux-note','HELLO')
        # Curses/RFL apps must survive nested alternate screens.
        line('rfl-fm');visible('10 Quit');g.send(b'\x1b[21~');settle()
        pane_command('echo AFTER_COMMANDER','AFTER_COMMANDER')
        line('lynx file:///usr/share/lynx/start.html');visible('Welcome to Lynx');g.send(b'q');settle();g.send(b'y');settle()
        pane_command('echo AFTER_LYNX','AFTER_LYNX')
        line('rift /tmp/mux-note');visible('F10 Menu');g.send(b'\x11');settle()
        pane_command('echo AFTER_EDITOR','AFTER_EDITOR')
        # History is independent of the live screen and remains searchable.
        pane_command('seq 1 100','100')
        key(b'[');visible('HISTORY');g.send(b'/42\r');settle();visible('42')
        g.send(b'y');visible('Copied selection');line('cat > /tmp/mux-copy');key(b']');g.send(b'\r\x04');settle()
        pane_command('cat /tmp/mux-copy','42')
        # Detach, run something in the original shell, then attach preserved panes.
        key(b'd');g.raw(b'[mux: left session main]');g.raw(g.prompt)
        assert b'main' in g.shell('mux list')
        assert b'HELLO\n' in g.shell('cat /tmp/mux-note')
        assert b'42\n' in g.shell('cat /tmp/mux-copy')
        line('mux attach main');visible('1:work');visible('ZOOM');key(b'z');visible('RIGHT_PANE');settle()
        # Closing a pane confirms, then restores its sibling's geometry.
        key(b'x');visible('Close this pane');g.send(b'n');settle();key(b'x');visible('Close this pane');g.send(b'y');settle()
        key(b'd');g.raw(b'[mux: left session main]');g.raw(g.prompt)
        g.shell('mux kill main');settle()
        assert b'main' not in g.shell('mux list').split(b'\n',1)[-1].rsplit(g.prompt,1)[0]
        # A hung-up attacher releases its outer PTY while its server survives.
        line('ptysh mux new hangup muxcheck --hangup');g.raw(g.prompt)
        line('mux attach hangup');visible('MUX_HANGUP_SURVIVED');g.send(b'\x03')
        g.raw(b'[mux: left session hangup]');g.raw(g.prompt)
        # A new named session after teardown reuses PTYs and stale socket names.
        line('mux new again');visible('1:shell 1');pane_command('echo REUSED_OK','REUSED_OK')
        line('exit');g.raw(b'[mux: left session again]');g.raw(g.prompt)
        # Exercise a session above descriptor 15 and both configured ceilings.
        line('mux new capacity');visible('1:shell 1')
        for tab in range(1,9):
            if tab>1:key(b'c');visible(str(tab)+':shell '+str(tab))
            if tab<=4:key(b'%');settle()
        key(b'c');visible('Tab limit reached')
        key(b'%');visible('Pane limit reached')
        # A management connection can stop an attached session.
        line('mux kill capacity');g.raw(b'[mux: left session capacity]');g.raw(g.prompt)
        capacity=g.shell('muxcheck');assert b'MUXCHECK_OK 16 PTYs' in capacity,capacity
        g.shell('echo history=5 > $HOME/.muxrc; echo prefix=a >> $HOME/.muxrc')
        prefix=b'\x01'
        line('mux new configured');visible('^A:');pane_command('seq 1 100','100')
        key(b'[');visible('HISTORY');g.send(b'g');visible('1/27')
        g.send(b'\x1b');g.wait(lambda:'HISTORY' not in g.screen.text(),'Escape leaves scrollback')
        key(b'd');g.raw(b'[mux: left session configured]');g.raw(g.prompt)
        g.shell('mux kill configured')
        g.send(b'shutdown\r');g.raw(b'session stopped' if system=='fuse-x' else b'power off.')
        assert g.process.wait(timeout=15)==0
        if system=='fuse-x':
            sys.path.insert(0,str(ROOT/'fuse-x'))
            from test_support import check_root
            check_root(disk)
        else:run(['python3',PORT/'tools/fsck.py',disk])
        print(system+': PTY/fd capacity, tabs, splits, resize, job control, editors, history, detach and reuse passed',flush=True)
    finally:
        (ROOT/'build'/('mux-'+system+'.log')).write_bytes(g.log)
        g.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--system',choices=['both','fuzix','fuse-x'],default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    a=p.parse_args()
    for system in ['fuse-x','fuzix'] if a.system=='both' else [a.system]:
        with tempfile.TemporaryDirectory(prefix='mux-'+system+'-') as temp:exercise(a,system,Path(temp))

if __name__=='__main__':main()
