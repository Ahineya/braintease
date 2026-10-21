#!/usr/bin/env python3
"""Mouse adoption in real Rift/Music guests; never changes a user disk image."""
import argparse
import os
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Screen
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT / 'ports/music/tests'))
from test_wav import fixture

TERMCAP = dict(name='/etc/termcap', text='ansi|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')


class UI:
    def __init__(self, guest):
        self.guest = guest
        self.screen = Screen()
        self.consumed = 0

    def send(self, data):
        os.write(self.guest.master, data)

    def pointer(self, row, col, code=0, release=False):
        self.send(f'\x1b[<{code};{col+1};{row+1}{"m" if release else "M"}'.encode())

    def click(self, row, col):
        self.pointer(row, col)
        self.pointer(row, col, release=True)

    def wait(self, predicate):
        end = time.monotonic() + 90
        while True:
            self.guest.pump()
            self.screen.feed(self.guest.log[self.consumed:])
            self.consumed = len(self.guest.log)
            if predicate(self.screen.text()):
                return self.screen.text()
            assert self.guest.process.poll() is None and time.monotonic() < end, self.screen.text()

    def text(self, text):
        print('Waiting for ' + repr(text), flush=True)
        return self.wait(lambda value: text in value)


def artifact(target, name):
    return ROOT / (f'fuse-x/build/apps/{name}.fxe' if target == 'native' else f'build/fuzix/{name}.rxe')


def rift(target):
    with tempfile.TemporaryDirectory(prefix='rift-mouse-') as folder:
        guest = Guest(Path(folder), target, {'rift': artifact(target, 'rift')}, [TERMCAP,
            dict(name='/notes.txt', text='ab界cd\n\ttabbed\n' + ''.join(f'line {i}\n' for i in range(50))),
            dict(name='/other.txt', text='second document\n' + 'a' * 144 + '\n')], blocks=65536)
        ui = UI(guest)
        try:
            ui.send(b'rift /notes.txt /other.txt\r'); ui.text('ab界cd')
            # A wide glyph occupies two cells, but the cursor stays on a boundary.
            ui.click(2, 10); ui.text('1:3')
            ui.click(2, 11); ui.text('1:5')
            ui.click(3, 10); ui.text('2:1')
            ui.click(3, 11); ui.text('2:5')
            ui.click(2, 11)
            ui.send(b'iZ'); ui.text('INSERT'); ui.text('[+]')
            ui.click(2, 8); ui.text('INSERT')
            ui.send(b'Y\x1bu'); ui.text('NORMAL')
            ui.send(b'\x13'); ui.wait(lambda text: '[+]' not in text)
            # Drag becomes a vi visual selection; Esc returns to normal.
            ui.pointer(2, 7); ui.pointer(2, 10, 32); ui.pointer(2, 10, release=True)
            ui.text('VISUAL'); ui.send(b'\x1b'); ui.text('NORMAL')
            # Tab hit testing uses the same widths and paging as the painter.
            ui.click(1, 25); ui.text('second document')
            ui.click(4, 12); ui.text('2:78')
            ui.click(1, 2); ui.text('NORMAL /notes.txt')
            ui.pointer(3, 10, 65); ui.text('4:')
            # Menu header and shared choice dialog accept mouse activation.
            ui.click(0, 9); ui.text('Select command')
            ui.send(b'\x1b'); ui.text('NORMAL')
            # The footer Open action invokes the reusable file picker.
            ui.click(23, 21); ui.text('Open file')
            text = ui.wait(lambda text: any('other.txt' in line and ':' not in line for line in text.splitlines()))
            line = next(i for i, line in enumerate(text.splitlines()) if 'other.txt' in line and ':' not in line)
            ui.click(line, 12)
            ui.click(line, 12)
            ui.text('second document')
            # Click Help then the shared message footer to dismiss it.
            ui.click(23, 3); ui.text("programmer's editor")
            ui.click(20, 6); ui.text('NORMAL')
            ui.send(b'\x11'); guest.read(guest.prompt)
            result = guest.shell('cat /notes.txt')
            assert 'ab界Zcd' in result, result
            print(target + ': Rift Unicode caret/edit, visual drag, tabs, wheel, menus, file picker and help passed', flush=True)
        finally:
            if guest.process.poll() is None:
                guest.at = len(guest.log)
                ui.send(b'\x1b:qa!\r')
                try: guest.read(guest.prompt, 5)
                except AssertionError: pass
            guest.close()


def music(target):
    with tempfile.TemporaryDirectory(prefix='music-mouse-') as folder:
        track = Path(folder) / 'track.wav'
        track.write_bytes(fixture(8000, 1, 16, 8000 * 90)[0])
        guest = Guest(Path(folder), target, {'music': artifact(target, 'music')}, [TERMCAP,
            dict(name='/dev/pcm', mode=0o20666, device=0x500),
            dict(name='/library/a.wav', mode=0o100644, path=str(track)),
            dict(name='/library/b.wav', mode=0o100644, path=str(track))], blocks=65536)
        ui = UI(guest)
        try:
            ui.send(b'music /library\r'); ui.text('a.wav')
            ui.click(5, 5); ui.click(5, 5); ui.text('Playing WAV')
            ui.click(23, 10); ui.text('Paused')
            ui.click(23, 38); ui.text('Volume 80%')
            ui.pointer(17, 20, 64); ui.text('Volume 85%')
            ui.pointer(18, 20); time.sleep(.15); ui.pointer(18, 40, 32); ui.pointer(18, 40, release=True)
            ui.text('0:45')
            ui.click(22, 24); ui.text('Playlist 2')
            ui.click(5, 45); ui.text('[Playlist]')
            ui.pointer(5, 45, 64); ui.text('/library/a.wav')
            ui.click(23, 10); ui.text('Playing WAV')
            # Quit on press; don't inject a release into the restored shell.
            ui.pointer(23, 45); guest.read(guest.prompt)
            print(target + ': Music panes, playback/pause, volume/wheel, drag seek, playlist and quit passed', flush=True)
        finally:
            guest.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple', 'both'], default='native')
    parser.add_argument('--app', choices=['rift', 'music', 'both'], default='both')
    args = parser.parse_args()
    os.environ['RCC_NATIVE_AUDIO'] = 'off'
    for target in (['native', 'ripple'] if args.target == 'both' else [args.target]):
        if args.app in ('rift', 'both'): rift(target)
        if args.app in ('music', 'both'): music(target)
