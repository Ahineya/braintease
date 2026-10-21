#!/usr/bin/env python3
"""Floating tabs on real kernels, including a durable layout across OS reboot."""
import argparse
import fcntl
import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT/'ports/fuzix/tools'))
from test_mux_restore import Terminal


def run(target):
    build = ROOT/('fuse-x/build' if target == 'native' else 'build/fuzix')
    apps = {name: build/(f'apps/{name}.fxe' if target == 'native' else f'{name}.rxe') for name in ('mux', 'muxcheck')}
    files = [dict(name='/dev/proc', mode=0o20444, device=0x103)]
    for i in range(16):
        files += [dict(name=f'/dev/ptyp{i:x}', mode=0o20600, device=0x300+i),
                  dict(name=f'/dev/ttyp{i:x}', mode=0o20600, device=0x202+i)]
    with tempfile.TemporaryDirectory(prefix='mux-desktop-') as folder:
        directory = Path(folder)
        guest = Guest(directory, target, apps, files, blocks=65536)
        term = Terminal(guest)
        def pointer(x, y, code=0, release=False):
            term.send(f'\x1b[<{code};{x+1};{y+1}{"m" if release else "M"}'.encode())
        def click(x, y): pointer(x, y); pointer(x, y, release=True)
        def row(y, text, x):
            end = time.monotonic()+60
            while time.monotonic() < end:
                guest.pump();term.screen.feed(guest.log[term.at:]);term.at=len(guest.log)
                if ''.join(term.screen.lines[y])[x:x+len(text)] == text: return
            raise AssertionError(term.screen.text())
        def resize(width, height):
            term.screen.resize(height, width)
            fcntl.ioctl(guest.master, __import__('termios').TIOCSWINSZ, struct.pack('HHHH',height,width,0,0))
            os.kill(guest.process.pid, signal.SIGWINCH)
        try:
            term.line('mux');term.active=True;term.shown('1:shell 1')
            term.line('echo TILED_STAYS');term.shown('TILED_STAYS')
            term.key(b'f');term.shown('2:desktop 2 [D]')
            term.key(b't');term.shown('Window title:');term.send(b'\x15editor\r');row(1,'2 editor',1)
            term.configure('echo RESTORE_BACK; exec /bin/sh','/')
            term.line('echo FLOAT_BACK');term.shown('FLOAT_BACK')
            term.key(b'w');term.shown('3:shell')
            term.key(b't');term.shown('Window title:');term.send(b'\x15player\r');row(3,'3 player',5)
            term.configure('echo RESTORE_FRONT; exec /bin/sh','/')
            term.line('echo FLOAT_FRONT');term.shown('FLOAT_FRONT')
            term.line('muxcheck --size');term.shown('MUX_GEOMETRY 58 13')
            click(3,22);term.shown('FLOAT_BACK',absent='FLOAT_FRONT')
            pointer(12,1);pointer(22,3,32);pointer(22,3,release=True);row(3,'2 editor',11)
            pointer(69,17);pointer(75,20,32);pointer(75,20,release=True)
            term.line('muxcheck --size');term.shown('MUX_GEOMETRY 64 16')
            term.key(b'm');term.shown('Move window:');term.send(b'h\x1b[A\r');row(2,'2 editor',10)
            term.key(b'v');term.shown('Resize window:');term.send(b'hk\r')
            term.line('muxcheck --size');term.shown('MUX_GEOMETRY 63 15')
            term.key(b'z');term.line('muxcheck --size');term.shown('MUX_GEOMETRY 78 19')
            term.key(b'z');row(2,'2 editor',10)
            resize(10,5);row(4,' main',0);resize(80,24);row(2,'2 editor',10)
            term.key(b'1');term.shown('TILED_STAYS',absent='editor')
            term.key(b'2');row(2,'2 editor',10)
            term.key(b'S');term.shown('Saved main;')
            term.leave()
            saved = guest.shell('cat $HOME/.mux/main')
            assert 'MUX-SESSION 4' in saved and 'F 1\n' in saved and 'W 1 9 2 65 17 1 ' in saved, saved
            print(target+': desktop/tiled coexistence, taskbar raise, drag/resize, keyboard, zoom and tiny-terminal recovery passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-desktop-'+target+'.ansi')).write_bytes(guest.log)
            if term.active: term.leave()
            guest.close()
        guest = Guest(directory,target,apps,reuse=True,blocks=65536)
        term = Terminal(guest)
        try:
            term.line('mux restore main');term.active=True
            term.shown('RESTORE_BACK',absent='RESTORE_FRONT');row(2,'2 editor',10)
            term.line('muxcheck --size');term.shown('MUX_GEOMETRY 63 15')
            click(60,22);term.shown('RESTORE_FRONT')
            # Mouse close is immediate; remaining windows must stay movable/resizable.
            click(61,3);row(2,'2 editor',10)
            pointer(17,2);pointer(16,2,32);pointer(16,2,release=True);row(2,'2 editor',9)
            pointer(72,18);pointer(71,17,32);pointer(71,17,release=True)
            term.line('muxcheck --size');term.shown('MUX_GEOMETRY 62 14')
            term.leave()
            guest.shell('mux kill main')
            print(target+': reboot restored desktop geometry, titles, stacking, focus and commands; immediate title-button close followed by drag/resize passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-desktop-'+target+'.ansi')).write_bytes(guest.log)
            if term.active: term.leave()
            guest.close()


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['native','ripple','both'],default='native')
    args=p.parse_args()
    for target in (['native','ripple'] if args.target=='both' else [args.target]): run(target)
