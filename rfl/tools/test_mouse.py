#!/usr/bin/env python3
"""RFL mouse events/widgets through host adapters and a real Fuse-X process."""
from pathlib import Path
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Screen
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def host():
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    child = subprocess.Popen([str(ROOT / 'build/tui-mouse/mouse-demo')],
                             stdin=slave, stdout=slave, stderr=slave, env=dict(os.environ, TERM='xterm'))
    os.close(slave)
    screen = Screen()
    def wait(text):
        end = time.monotonic() + 20
        while text not in screen.text():
            assert time.monotonic() < end, screen.text()
            if select.select([master], [], [], .1)[0]:
                screen.feed(os.read(master, 65536))
    try:
        wait('Count: 0')
        os.write(master, b'\x1b[<0;6;5M\x1b[<0;6;5m')
        wait('Count: 1')
        os.write(master, b'q')
        assert child.wait(timeout=10) == 0
        print('RFL host: mouse ABI, hit testing and click/cleanup passed', flush=True)
    finally:
        if child.poll() is None:
            child.kill(); child.wait()
        os.close(master)


def commander():
    with tempfile.TemporaryDirectory(prefix='rfl-mouse-') as folder:
        termcap = dict(name='/etc/termcap', text='ansi|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')
        files = [termcap, *[dict(name='/left/'+n+'.txt', text=n) for n in ('a', 'b', 'c', 'd')], dict(name='/right/other.txt', text='right')]
        guest = Guest(Path(folder), 'native', {'rfl-fm': ROOT/'fuse-x/build/apps/rfl-fm.fxe'}, files)
        screen = Screen(); consumed = 0
        def wait(test):
            nonlocal consumed
            end = time.monotonic() + 25
            while True:
                guest.pump(); screen.feed(guest.log[consumed:]); consumed = len(guest.log)
                if test(): return
                assert guest.process.poll() is None and time.monotonic() < end, screen.text()
        def send(data): os.write(guest.master, data)
        try:
            send(b'rfl-fm /left /right\r'); wait(lambda: screen.text().splitlines()[-2].strip() == 'Selected: /')
            send(b'\x1b[<0;4;7M\x1b[<0;4;7m'); wait(lambda: 'Selected: /left/b.txt' in screen.text())
            send(b'\x1b[<64;4;7M'); wait(lambda: screen.text().splitlines()[-2].strip() == 'Selected: /')
            send(b'\x1b[<0;51;6M\x1b[<0;51;6m'); wait(lambda: 'Selected: /right/other.txt' in screen.text())
            send(b'\x1b[<0;41;3M\x1b[<32;51;3M\x1b[<0;51;3m')
            wait(lambda: screen.text().splitlines()[1].find('[ /right ]') == 52)
            # Commander also uses shared mouse-aware theme and About dialogs.
            send(b'\x01'); wait(lambda: 'About RFL Commander' in screen.text())
            send(b'\x1b[<0;8;21M\x1b[<0;8;21m')
            wait(lambda: 'About RFL Commander' not in screen.text())
            send(b'\x1b[20~'); wait(lambda: 'Choose theme (session only)' in screen.text())
            send(b'\x1b[<0;8;12M\x1b[<0;8;12m')
            wait(lambda: 'Choose theme (session only)' not in screen.text())
            # Last function button is the same Quit action as F10.
            send(b'\x1b[<0;67;24M')
            guest.read(guest.prompt)
            print('Commander guest: click selection/focus, wheel, divider drag and function-button quit passed', flush=True)
        finally:
            guest.close()


def rvm():
    with tempfile.TemporaryDirectory(prefix='rfl-mouse-rvm-') as folder:
        termcap = dict(name='/etc/termcap', text='ansi|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')
        guest = Guest(Path(folder), 'ripple', {'mouse-demo': ROOT/'build/tui-mouse/mouse-demo.rxe'}, [termcap])
        screen = Screen(); consumed = 0
        def wait(text):
            nonlocal consumed
            end = time.monotonic() + 60
            while text not in screen.text():
                guest.pump(); screen.feed(guest.log[consumed:]); consumed = len(guest.log)
                assert guest.process.poll() is None and time.monotonic() < end, screen.text()
        try:
            os.write(guest.master, b'mouse-demo\r'); wait('Count: 0')
            os.write(guest.master, b'\x1b[<0;6;5M\x1b[<0;6;5m'); wait('Count: 1')
            os.write(guest.master, b'q'); guest.read(guest.prompt)
            print('RFL RVM: mouse event layout, hit testing and click/cleanup passed', flush=True)
        finally:
            guest.close()


if __name__ == '__main__':
    if sys.platform == 'darwin': host()
    commander()
    if (ROOT/'build/tui-mouse/mouse-demo.rxe').exists(): rvm()
