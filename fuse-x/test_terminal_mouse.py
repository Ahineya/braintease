#!/usr/bin/env python3
"""Real GX/Nano-X pointer -> PTY -> curses, including Shift-drag selection."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    build = ROOT / 'fuse-x/build'
    with tempfile.TemporaryDirectory(prefix='terminal-mouse-') as folder:
        work = Path(folder)
        mailbox, frames = work / 'event.json', work / 'frames'
        probe = work / 'mousecheck.fxe'
        subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'),
                        str(ROOT / 'ports/fuzix/tools/mousecheck.c'), '--curses',
                        '--build', str(build), '-o', str(probe)], check=True)
        apps = {n: build / 'apps' / (n + '.fxe') for n in ('nano-X', 'gterm', 'nxterm', 'nxhostcheck', 'mux')}
        apps['mousecheck'] = probe
        devices = [dict(name=f'/dev/{name}{i:x}', mode=0o20600, device=base+i)
                   for i in range(16) for name, base in [('ptyp', 0x300), ('ttyp', 0x202)]]
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_EVENT=str(mailbox), FUSE_X_FRAMES=str(frames), NANOX_KEEP_TTY='1')
        try:
            termcap = dict(name='/etc/termcap', text='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n')
            guest = Guest(work, 'native', apps, [*devices, termcap, dict(name='/dev/gfx', mode=0o20600, device=0x400)], blocks=65536)
        finally:
            os.environ.clear(); os.environ.update(saved)

        def wait(test, seconds=25):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                guest.pump()
                result = test()
                if result:
                    return result
                assert guest.process.poll() is None, guest.log[-4000:]
                time.sleep(.02)
            raise AssertionError(guest.log[-6000:].decode(errors='replace'))

        def windows():
            try:
                return json.loads((frames / 'windows.json').read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                return []

        def event(wid, **values):
            wait(lambda: not mailbox.exists())
            temporary = mailbox.with_suffix('.tmp')
            temporary.write_text(json.dumps(dict(window=wid, **values)))
            temporary.replace(mailbox)
            wait(lambda: not mailbox.exists())

        try:
            guest.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            guest.shell('nano-X -p &')
            if b'Nano-X ready' not in guest.log:
                guest.read(b'Nano-X ready')
            for terminal, cols, rows in [('gterm', 80, 30), ('nxterm', 64, 20)]:
                path = '/tmp/mouse-' + terminal
                guest.shell(f'{terminal} mousecheck {path} &')
                guest.shell('term=$!')
                window = wait(lambda: next((w for w in windows() if w['title'] == 'Terminal' and w['visible']), None))
                wid = window['id']; cw = window['width'] // cols; ch = window['height'] // rows
                wait(lambda: 'READY' in guest.shell('cat ' + path))
                def pointer(kind, x, y, **values):
                    event(wid, input=dict(type=kind, x=x*cw+cw//2, y=y*ch+ch//2, **values))
                pointer('button', 3, 2, button=0, down=True)
                pointer('motion', 4, 2)
                pointer('button', 4, 2, button=0, down=False)
                wait(lambda: 'M 3 2 2' in guest.shell('cat ' + path) and 'M 4 2 1' in guest.log[-2000:].decode(errors='replace'))
                pointer('motion', 5, 3)
                wait(lambda: 'M 5 3 536870912' in guest.shell('cat ' + path))
                event(wid, input=dict(type='wheel', x=0, y=65536))
                wait(lambda: '65536' in guest.shell('cat ' + path))
                before = guest.shell('cat ' + path).count('\nM ')
                event(wid, input=dict(type='key', key=225, down=True))
                pointer('button', 0, 0, button=0, down=True)
                pointer('motion', 5, 0)
                pointer('button', 5, 0, button=0, down=False)
                event(wid, input=dict(type='key', key=225, down=False))
                event(wid, action=1, clipboard='SELECTION_FAILED')
                assert 'CLIPBOARD_CHECK 0' in guest.shell('nxhostcheck clipboard get /tmp/mouse-copy')
                assert 'Select' in guest.shell('cat /tmp/mouse-copy')
                assert guest.shell('cat ' + path).count('\nM ') == before
                event(wid, input=dict(type='key', key=20, down=True))
                event(wid, input=dict(type='key', key=20, down=False))
                wait(lambda: 'CLOSED' in guest.shell('cat ' + path))
                guest.shell('wait $term')
                wait(lambda: not any(w['title'] == 'Terminal' for w in windows()))
                print(terminal + ': click, drag, release, hover, wheel, Shift-drag clipboard selection and clean exit passed', flush=True)
        finally:
            guest.close()
            output = ROOT / 'build/tui-mouse'; output.mkdir(parents=True, exist_ok=True)
            (output / 'terminal-mouse.log').write_bytes(guest.log)


if __name__ == '__main__':
    main()
