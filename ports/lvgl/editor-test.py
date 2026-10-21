#!/usr/bin/env python3
"""Exercise the graphical editor through GX input on a fresh offline guest."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import time

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gui', PORT/'gui-test.py')
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl')
    parser.add_argument('--references', type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    references = (args.references or build/'host').resolve()
    out = build/'editor-tests'
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    sample = PORT/'examples/editor-demo.c'
    application = build/'lvedit.fxe'
    original = sample.read_bytes()
    saved = '/data/edited-Привіт.txt'
    x = gui.GUI(out, {'lvedit': application, 'lvgallery': build/'lvgallery.fxe'}, [
        dict(name=str(sample), path=str(sample), mode=0o100644),
        dict(name='/data/binary.txt', text='not\0text', mode=0o100644),
        dict(name='/data/dialog-note.txt', text='Shared browser selection.\n', mode=0o100644)])
    g = x.g

    def launch(path):
        at = len(g.log)
        g.shell('lvedit --trace '+shlex.quote(path)+' &')
        g.shell('editor=$!')
        x.logged('EDIT_READY ', at=at)
        return x.wait(lambda: x.window('Text Editor'))['id']

    def changed(wid, text):
        at = len(g.log)
        x.send(wid, action=4)
        x.send(wid, action=2, clipboard=text)
        x.logged('EDIT_CHANGE bytes='+str(len(text.encode()))+' ', at=at)

    def path_dialog(wid, path):
        # The shared chooser scans its initial folder asynchronously; wait for
        # that small fixture listing before submitting a filename.
        x.frame(wid, width=880, height=600)
        x.send(wid, action=4)
        x.send(wid, action=2, clipboard=path)
        x.key(wid, 40)

    def button(wid, caption, at):
        pattern = rb'EDIT_RECT '+re.escape(caption.encode())+rb' (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)'
        values = x.wait(lambda: re.findall(pattern, g.log[at:]))[-1]
        px, py, width, height, density = map(int, values)
        x.click(wid, (px+width//2)//density, (py+height//2)//density)

    try:
        wid = launch(str(sample))
        for scale in (1, 2):
            x.send(wid, density=scale)
            expected = gui.ppm(references/f'editor-{scale}x.ppm')
            x.frame(wid, scale=scale, width=880, height=600)
            x.wait(lambda: gui.ppm(x.frames/f'{wid}.ppm') == expected)
            x.snapshot(wid, f'editor-{scale}x', scale=scale, width=880, height=600)
        x.send(wid, density=1)
        x.frame(wid, width=880, height=600)
        print('EDITOR_GUEST exact editor pixels passed at 1x/2x', flush=True)

        # One clipboard operation is one history entry, even for many wraps.
        paste = ('Привіт! A long paragraph with é, punctuation, and wrapped words. '*100+'\n')*2
        assert 12000 < len(paste.encode()) < 32768
        start = time.monotonic()
        changed(wid, paste)
        paste_seconds = time.monotonic()-start
        at = len(g.log)
        x.ctrl(wid, 29)
        x.logged('EDIT_CHANGE bytes='+str(len(original))+' dirty=0', at=at)
        at = len(g.log)
        x.ctrl(wid, 29, shift=True)
        x.logged('EDIT_CHANGE bytes='+str(len(paste.encode()))+' dirty=1', at=at)
        at = len(g.log)
        x.ctrl(wid, 22)
        x.logged('EDIT_SAVE '+str(sample), at=at)
        assert paste in g.shell('cat '+shlex.quote(str(sample)))

        # The public clipboard buffer includes NUL; the GX payload supports
        # exactly 32768 octets. Copy/cut and paste must preserve that boundary.
        boundary = ('Exact 32 KiB clipboard boundary. '*1100)[:32768]
        assert len(boundary.encode()) == 32768
        changed(wid, boundary)
        at = len(g.log)
        x.ctrl(wid, 22)
        x.logged('EDIT_SAVE '+str(sample), at=at)
        assert boundary in g.shell('cat '+shlex.quote(str(sample)))
        x.send(wid, action=4)
        x.send(wid, action=1)
        at = len(g.log)
        x.send(wid, action=3)
        x.logged('EDIT_CHANGE bytes=0 dirty=1', at=at)
        at = len(g.log)
        x.ctrl(wid, 29)
        x.logged('EDIT_CHANGE bytes=32768 dirty=0', at=at)
        x.send(wid, action=4)
        at = len(g.log)
        x.send(wid, action=2)
        x.logged('EDIT_CHANGE bytes=32768 ', at=at)
        at = len(g.log)
        x.ctrl(wid, 22)
        x.logged('EDIT_SAVE '+str(sample), at=at)
        assert boundary in g.shell('cat '+shlex.quote(str(sample)))
        changed(wid, paste)
        at = len(g.log)
        x.ctrl(wid, 22)
        x.logged('EDIT_SAVE '+str(sample), at=at)

        print('EDITOR_GUEST Unicode paste, grouped undo/redo and exact 32 KiB clipboard round trip passed', flush=True)

        # Per-tab state and a new Unicode path survive a clean process restart.
        x.ctrl(wid, 17)
        at = len(g.log)
        short = 'Збережено: é, 👨‍👩‍👧‍👦, 🇺🇦.\nSecond line.\n'
        changed(wid, short)
        x.ctrl(wid, 22)
        x.logged('EDIT_DIALOG save', at=at)
        x.snapshot(wid, 'save-browser', width=880, height=600)
        path_dialog(wid, saved)
        x.logged('EDIT_SAVE '+saved, at=at)
        assert short in g.shell('cat '+shlex.quote(saved))
        at = len(g.log)
        x.ctrl(wid, 43)
        x.logged('EDIT_TAB name=editor-demo.c dirty=0', at=at)
        x.ctrl(wid, 43)
        settings = g.shell('cat /data/home/.lvedit')
        assert 'version=1\nwrap=1' in settings and saved.encode().hex() in settings

        # A failed open keeps the current document, with an editable error.
        at = len(g.log)
        x.ctrl(wid, 18)
        x.logged('EDIT_DIALOG open', at=at)
        path_dialog(wid, '/data/binary.txt')
        x.pause(.2)
        assert b'EDIT_OPEN /data/binary.txt' not in g.log[at:]
        x.key(wid, 41)
        x.snapshot(wid, 'after-failed-open', width=880, height=600)

        # Browse a directory and activate a real row inside the shared modal.
        # The fixture contains '..', home/, binary.txt, dialog-note.txt, saved.
        at = len(g.log)
        x.ctrl(wid, 18)
        x.logged('EDIT_DIALOG open', at=at)
        x.frame(wid, width=880, height=600)
        x.send(wid, action=4)
        x.send(wid, action=2, clipboard='/data')
        x.key(wid, 40)
        x.snapshot(wid, 'open-browser', width=880, height=600)
        assert len(x.windows()) == 1 and not x.window('Select File')
        x.send(wid, resize=[540, 360])
        x.snapshot(wid, 'open-browser-small', width=540, height=360)
        x.send(wid, resize=[880, 600])
        x.frame(wid, width=880, height=600)
        x.key(wid, 74)
        for _ in range(3):
            x.key(wid, 81)
        x.snapshot(wid, 'open-browser-selected', width=880, height=600)
        at = len(g.log)
        x.key(wid, 40)
        x.logged('EDIT_OPEN /data/dialog-note.txt', at=at)
        x.snapshot(wid, 'browser-selection', width=880, height=600)

        # Resize/density and settled idle preserve the active document.
        for width, height, scale in ((540, 360, 1), (1470, 900, 2), (880, 600, 1)):
            x.send(wid, density=scale)
            x.send(wid, resize=[width, height])
            x.frame(wid, width=width, height=height, scale=scale)
        x.pause(.4)
        stamp = (x.frames/f'{wid}.ppm').stat().st_mtime_ns
        x.pause(.8)
        assert (x.frames/f'{wid}.ppm').stat().st_mtime_ns == stamp, 'Idle editor repaints'
        x.send(wid, close=True)
        x.wait(lambda: not x.window('Text Editor'))
        assert 'EDIT_STATUS:0' in g.shell('wait $editor; echo EDIT_STATUS:$?')

        wid = launch(saved)
        at = len(g.log)
        x.text(wid, '!')
        x.logged('dirty=1', at=at)
        at = len(g.log)
        x.send(wid, close=True)
        x.logged('EDIT_DIALOG unsaved', at=at)
        x.key(wid, 41)
        x.pause(.2)
        assert x.window('Text Editor'), 'Cancel closed a dirty document'
        at = len(g.log)
        x.send(wid, close=True)
        x.logged('EDIT_DIALOG unsaved', at=at)
        button(wid, 'Discard', at)
        x.wait(lambda: not x.window('Text Editor'))
        assert 'EDIT_STATUS:0' in g.shell('wait $editor; echo EDIT_STATUS:$?')
        assert short in g.shell('cat '+shlex.quote(saved))
        print('EDITOR_GUEST file, tab, settings, resize, idle and native-close workflows passed', flush=True)
        at = len(g.log)
        g.shell('lvgallery --trace &')
        g.shell('gallery=$!')
        gallery = x.wait(lambda: x.window('Graphite Gallery'))['id']
        x.key(gallery, 66)
        x.logged('GALLERY_EDITOR_READY tabs=2', at=at)
        for scale in (1, 2):
            x.send(gallery, density=scale)
            # Live density changes publish caret status. The reference depicts
            # a fresh page (Ready), so recreate that same state at each density.
            x.key(gallery, 65)
            x.key(gallery, 66)
            x.frame(gallery, width=760, height=560, scale=scale)
            expected = gui.ppm(references/f'gallery-editor-{scale}x.ppm')
            x.snapshot(gallery, f'gallery-editor-{scale}x', width=760, height=560, scale=scale)
            x.wait(lambda: gui.ppm(x.frames/f'{gallery}.ppm') == expected)
        x.send(gallery, density=1)
        x.frame(gallery, width=760, height=560)
        rects = re.findall(rb'GALLERY_RECT editor (\d+) (\d+) (\d+) (\d+)', g.log[at:])
        left, top, right, bottom = map(int, rects[-1])
        x.click(gallery, left+20, top+15)
        at = len(g.log)
        x.text(gallery, 'Z')
        x.logged('GALLERY_EDITOR bytes=', at=at)
        x.key(gallery, 65)
        x.key(gallery, 66)
        x.frame(gallery, width=760, height=560)
        expected = gui.ppm(references/'gallery-editor-1x.ppm')
        x.wait(lambda: gui.ppm(x.frames/f'{gallery}.ppm') == expected)
        x.send(gallery, close=True)
        x.wait(lambda: not x.window('Graphite Gallery'))
        assert 'GALLERY_STATUS:0' in g.shell('wait $gallery; echo GALLERY_STATUS:$?')
        assert not any(error in g.log for error in (b'native runtime error', b'panic:', b'graphics operation failed'))
        report = dict(status='passed', executable_sha256=hashlib.sha256(application.read_bytes()).hexdigest(),
                      gallery_executable_sha256=hashlib.sha256((build/'lvgallery.fxe').read_bytes()).hexdigest(),
                      paste_octets=len(paste.encode()), paste_seconds=round(paste_seconds, 3),
                      clipboard_boundary_octets=len(boundary.encode()),
                      checks=['exact host/guest editor pixels at 1x/2x', 'large Unicode paste and grouped undo/redo',
                              'exact 32768-octet clipboard paste/copy/cut/undo round trip',
                              'atomic saves and Unicode filenames', 'independent tabs and HOME recents',
                              'failed binary open preserves document',
                              'shared same-window directory browser, keyboard file selection and small-window layout', 'resize and density transitions',
                              'idle publication', 'restart and native close cancel/discard',
                              'gallery Editor pixel parity, typing and page teardown/recreation'])
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print('Graphical editor: input, files, tabs, paste, density and close workflows passed', flush=True)
    finally:
        x.close()


if __name__ == '__main__':
    main()
