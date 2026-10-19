#!/usr/bin/env python3
"""Move a live pane, save recipes, stop the OS, then restore on the same disk."""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Screen


class Terminal:
    def __init__(self, guest):
        self.guest, self.screen, self.at = guest, Screen(), len(guest.log)
        self.active = False

    def send(self, data): os.write(self.guest.master, data)
    def line(self, text): self.send(text.encode() + b'\r')
    def key(self, text): self.send(b'\x02' + text)

    def shown(self, marker, absent=None, timeout=90):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.guest.pump()
            self.screen.feed(bytes(self.guest.log[self.at:])); self.at = len(self.guest.log)
            if marker in self.screen.text() and (absent is None or absent not in self.screen.text()): return
        raise AssertionError(f'Missing {marker!r}, excluding {absent!r}\n' + self.screen.text())

    def configure(self, command, directory):
        self.key(b'C'); self.shown('Restart command')
        self.send(b'\x15' + command.encode() + b'\r'); self.shown('Restart command updated')
        self.key(b'D'); self.shown('Restart directory:')
        self.send(b'\x15' + directory.encode() + b'\r'); self.shown('Restart directory updated')

    def leave(self):
        self.key(b'd'); self.guest.read(b'[mux: left session main]', 60)
        self.guest.read(self.guest.prompt, 30); self.active = False


def run(target):
    build = ROOT / ('fuse-x/build' if target == 'native' else 'build/fuzix')
    ext = 'fxe' if target == 'native' else 'rxe'
    apps = {name: build / (f'apps/{name}.{ext}' if target == 'native' else f'{name}.{ext}')
            for name in ('mux', 'muxcheck', 'cp', 'chmod', 'pwd')}
    files = [dict(name='/dev/proc', mode=0o20444, device=0x103),
             dict(name='/work/left/README', text='left'), dict(name='/work/right/README', text='right'),
             dict(name='/work/folder name/README', text='third')]
    for i in range(16):
        files += [dict(name=f'/dev/ptyp{i:x}', mode=0o20600, device=0x300 + i),
                  dict(name=f'/dev/ttyp{i:x}', mode=0o20600, device=0x202 + i)]
    # Structurally invalid graph with an observable command, rejected before spawn.
    bad = ('MUX-SESSION 1\nO 200 2 ' + '/bin/sh'.encode().hex() + '\nA 0\nT 0 0 0 0 626164\nP 0 0 1 ' +
           '/'.encode().hex() + ' ' + 'echo BAD > /work/UNEXPECTED'.encode().hex() +
           '\nN 0 -1 0 1 -1 1 50\nN 1 0 -1 -1 0 0 50\nEND\n')
    files.append(dict(name='/bad-snapshot', text=bad))
    with tempfile.TemporaryDirectory(prefix='mux-reboot-', dir='/tmp') as temporary:
        work = Path(temporary)
        guest = Guest(work, target, apps, files, blocks=24576, disk_sectors=131072)
        term = Terminal(guest)
        try:
            term.line('mux'); term.active = True; term.shown('1:shell 1')
            term.configure('echo RESTORED_LEFT; exec /bin/sh', '/work/left')
            term.line('echo LIVE_LEFT'); term.shown('LIVE_LEFT')
            term.key(b'%'); term.shown('pane 2')
            term.configure('echo RESTORED_RIGHT; exec /bin/sh', '/work/right')
            term.line('echo LIVE_RIGHT'); term.shown('LIVE_RIGHT')
            term.key(b'"'); term.shown('pane 3')
            term.configure('echo RESTORED_THREE; exec /bin/sh', '/work/folder name')
            term.line('KEEP=original; echo LIVE_THREE'); term.shown('LIVE_THREE')
            term.key(b'!'); term.shown('2:pane 3')
            term.line('echo STILL_$KEEP'); term.shown('STILL_original')
            term.key(b'1'); term.shown('LIVE_LEFT'); term.shown('LIVE_RIGHT')
            term.key(b','); term.shown('Rename tab:'); term.send(b'\x15work\r'); term.shown('1:work')
            term.key(b'+'); term.key(b'z'); term.shown('ZOOM')
            term.key(b'2'); term.shown('STILL_original')
            term.key(b','); term.shown('Rename tab:'); term.send(b'\x15agent\r'); term.shown('2:agent')
            term.key(b'S'); term.shown('Saved main;')
            # Management saves work from a pane while another client is attached.
            term.line('mux save main; echo ATTACHED_SAVE:$?'); term.shown('ATTACHED_SAVE:0')
            term.leave()
            assert 'main' in guest.shell('mux saved')
            assert 'ACTIVE:1' in guest.shell('mux restore main; echo ACTIVE:$?')
            assert 'DETACHED_SAVE:0' in guest.shell('mux save main; echo DETACHED_SAVE:$?')
            guest.shell('cp /bad-snapshot /.mux/bad; chmod 600 /.mux/bad')
            result = guest.shell('mux restore bad; echo INVALID:$?')
            assert 'INVALID:1' in result and 'invalid' in result, result
            assert 'EFFECT:1' in guest.shell('cat /work/UNEXPECTED; echo EFFECT:$?')
            print(target + ': live pane promotion, recipes, attached/detached save and invalid-layout rejection passed', flush=True)
        finally:
            if term.active: term.leave()
            guest.close()

        # A genuine kernel reboot, not just detaching the terminal or mux server.
        guest = Guest(work, target, apps, files, blocks=24576, disk_sectors=131072, reuse=True)
        term = Terminal(guest)
        try:
            assert 'main' in guest.shell('mux saved')
            term.line('mux restore main'); term.active = True
            term.shown('2:agent'); term.shown('RESTORED_THREE')
            term.line('pwd'); term.shown('/work/folder name')
            term.line('echo STATE_$KEEP:fresh'); term.shown('STATE_:fresh')
            term.key(b'1'); term.shown('1:work'); term.shown('ZOOM'); term.shown('RESTORED_LEFT')
            term.line('pwd'); term.shown('/work/left')
            term.key(b'z'); term.shown('RESTORED_RIGHT')
            term.line('muxcheck --size'); term.shown('MUX_GEOMETRY 43 22')
            term.key(b'o'); term.line('pwd'); term.shown('/work/right')
            term.line('muxcheck --size'); term.shown('MUX_GEOMETRY 36 22')
            term.leave()
            assert 'RESTORE_SAVED:0' in guest.shell('mux save main; echo RESTORE_SAVED:$?')
            print(target + ': real OS reboot restored tab names, selected tab, zoom, split ratios, focus, directories and commands', flush=True)
        finally:
            if term.active: term.leave()
            guest.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    run(parser.parse_args().target)
