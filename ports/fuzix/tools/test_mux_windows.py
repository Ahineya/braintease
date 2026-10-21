#!/usr/bin/env python3
"""Real guest coverage for Run, program titles, minimize and persisted desktop state."""
import argparse
from pathlib import Path
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0,str(ROOT/'ports/fuzix/tools'))
from test_mux_restore import Terminal


def run(target):
    build=ROOT/('fuse-x/build' if target=='native' else 'build/fuzix')
    apps={name:build/(f'apps/{name}.fxe' if target=='native' else f'{name}.rxe') for name in ('mux','muxcheck')}
    files=[dict(name='/dev/proc',mode=0o20444,device=0x103),dict(name='/work/folder name/README',text='cwd fixture')]
    for i in range(16):
        files += [dict(name=f'/dev/ptyp{i:x}',mode=0o20600,device=0x300+i),dict(name=f'/dev/ttyp{i:x}',mode=0o20600,device=0x202+i)]
    with tempfile.TemporaryDirectory(prefix='mux-windows-') as folder:
        work=Path(folder)
        def boot(reuse=False):return Guest(work,target,apps,files,blocks=65536,reuse=reuse)
        guest=boot();term=Terminal(guest)
        def row(y,text,x):
            until=time.monotonic()+60
            while time.monotonic()<until:
                guest.pump();term.screen.feed(guest.log[term.at:]);term.at=len(guest.log)
                if ''.join(term.screen.lines[y])[x:x+len(text)]==text:return
            raise AssertionError(f'row {y}: {text!r}\n'+term.screen.text())
        def click(x,y):
            term.send(f'\x1b[<0;{x+1};{y+1}M\x1b[<0;{x+1};{y+1}m'.encode())
        try:
            term.line('mux');term.active=True;term.shown('1:shell 1')
            term.key(b'f');term.shown('2:desktop 2 [D]')
            term.line("cd '/work/folder name'; muxcheck --workspace FIRST");term.shown('WORKSPACE_READY');row(1,'2 muxcheck',1)
            term.key(b'w');row(3,'3 shell',5)
            term.line('muxcheck --workspace SECOND');term.shown('WORKSPACE_ARG SECOND');row(3,'3 muxcheck',5)
            term.key(b'M');row(1,'2 muxcheck',1);term.shown('WORKSPACE_ARG FIRST',absent='WORKSPACE_ARG SECOND')
            click(60,22);row(3,'3 muxcheck',5);term.shown('WORKSPACE_ARG SECOND')
            term.key(b'M');term.key(b'M');term.shown('All windows minimized')
            term.line('quit')  # Hidden programs must not receive ordinary input.
            click(10,22);row(1,'2 muxcheck',1);term.shown('WORKSPACE_ARG FIRST')
            term.key(b'e');term.shown('Run program')
            term.send(b'\x1b[200~muxcheck --workspace CANCELLED\n\x1b[201~');term.shown('CANCELLED')
            term.send(b'\x1b');term.shown('WORKSPACE_ARG FIRST',absent='Run program')
            term.key(b'e');term.shown('Run program');term.send("muxcheck --workspace 'Run Ж'\r".encode())
            term.shown('WORKSPACE_ARG Run Ж');row(5,'4 muxcheck',9)
            term.key(b't');term.shown('Window title:');term.send(b'\x15tools\r');row(5,'4 tools',9)
            term.line('quit');term.shown('WORKSPACE_EXIT');row(5,'4 tools',9)
            term.line('muxcheck --workspace NAMED');term.shown('WORKSPACE_ARG NAMED');row(5,'4 tools',9)
            term.key(b'S');term.shown('Saved main;');term.leave()
            snapshot=guest.shell('cat $HOME/.mux/default')
            assert '/work/folder name'.encode().hex() in snapshot
            assert 'MUX-SESSION 4' in snapshot and 'V 2 1\n' in snapshot and 'V 3 2\n' in snapshot,snapshot
            print(target+': automatic/manual titles, minimize/restore, all-hidden input isolation, Run modal/cancel/paste and desktop launch passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-windows-'+target+'-live.ansi')).write_bytes(guest.log)
            if term.active:term.leave()
            guest.close()
        guest=boot(True);term=Terminal(guest)
        try:
            term.line('mux');term.active=True;term.shown('WORKSPACE_ARG NAMED');row(5,'4 tools',9)
            term.shown('3:muxcheck',absent='WORKSPACE_ARG SECOND')
            click(36,22);row(3,'3 muxcheck',5);term.shown('WORKSPACE_ARG SECOND')
            term.key(b'1');term.shown('1:shell 1')
            term.key(b'e');term.shown('Run program');term.send(b'muxcheck --workspace TILED\r')
            term.shown('3:muxcheck');term.shown('WORKSPACE_ARG TILED')
            term.line('quit');term.shown('WORKSPACE_EXIT')
            term.key(b'e');term.shown('Run program');term.send(b'mux-no-such-program\r');term.shown('not found')
            term.line('echo LAUNCH_SHELL_OK');term.shown('LAUNCH_SHELL_OK')
            term.leave();guest.shell('mux kill main')
            print(target+': reboot retained minimized state and manual titles; Run from tiled tab and failed-launch shell recovery passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-windows-'+target+'-reboot.ansi')).write_bytes(guest.log)
            if term.active:term.leave()
            guest.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['native','ripple','both'],default='native')
    args=p.parse_args()
    for target in (['native','ripple'] if args.target=='both' else [args.target]):run(target)
