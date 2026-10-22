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
import shutil
import struct
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gui', PORT/'gui-test.py')
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def save_png(path, image):
    width, height, pixels = image
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data)&0xffffffff)
    rows = b''.join(b'\0'+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))+
                     chunk(b'IDAT', zlib.compress(rows))+chunk(b'IEND', b''))


class EditorGUI(gui.GUI):
    def __init__(self, out, applications, files):
        self.out = Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='graphite-editor-')
        work = Path(self.temp.name)
        self.frames, self.mailbox = work/'frames', work/'event.json'
        saved = dict(os.environ)
        os.environ.update(FUSE_X_FRAMES=str(self.frames), FUSE_X_TEST_EVENT=str(self.mailbox), FUSE_X_DESKTOP='0')
        try:
            self.g = gui.Guest(work, 'native', applications,
                               [dict(name='/dev/gfx', mode=0o20600, device=0x400),
                                dict(name='/data/home', directory=True, mode=0o40755), *files], blocks=131072)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell('HOME=/data/home; export HOME; unset LVGL_SCALE')

    def snapshot(self, wid, name, **geometry):
        result = super().snapshot(wid, name, **geometry)
        save_png(self.out/(name+'.png'), gui.ppm(result))
        return result

    def close(self):
        saved = self.out/'last-frames'
        saved.mkdir(exist_ok=True)
        for path in self.frames.glob('*'):
            if path.suffix in ('.json', '.ppm'):
                shutil.copyfile(path, saved/path.name)
        super().close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl')
    parser.add_argument('--references', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--new-only', action='store_true', help='Run the new tab, syntax and gallery workflows only')
    args = parser.parse_args()
    build = args.build.resolve()
    references = (args.references or build/'host').resolve()
    out = (args.output or build/'editor-tests').resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    sample = PORT/'examples/editor-demo.c'
    application = build/'lvedit.fxe'
    original = sample.read_bytes()
    saved = '/data/edited-Привіт.txt'
    tab_paths = {scale: [f'/editor-tabs/{name}-{scale}.txt' for name in ('alpha', 'beta', 'gamma')]
                 for scale in (1, 2)}
    # The private C16 V3 fixture filesystem has a 30-octet filename limit.
    long_paths = [f'/editor-tabs/'+(f'0123456789-long-title-{i:02}.txt' if i % 2 else
                                   f'дуже-довгий-{i:02}.txt') for i in range(1, 7)]
    assert all(len(Path(path).name.encode()) <= 30 for path in long_paths)
    # The image builder maps JS characters to C16 cells. POSIX pathnames use
    # UTF-8 octet cells, represented by Latin-1 characters in its manifest API.
    files = [
        dict(name=str(sample), path=str(sample), mode=0o100644),
        dict(name='/data/binary.txt', text='not\0text', mode=0o100644),
        dict(name='/data/dialog-note.txt', text='Shared browser selection.\n', mode=0o100644),
        *[dict(name=path.encode('utf-8').decode('latin1'), text=Path(path).stem+' contents.\n', mode=0o100644)
          for paths in (*tab_paths.values(), long_paths) for path in paths]]
    old_kernel = os.environ.get('FUSE_X_TEST_KERNEL')
    if args.kernel:
        os.environ['FUSE_X_TEST_KERNEL'] = str(args.kernel.resolve())
    try:
        x = EditorGUI(out, {'lvedit': application, 'lvgallery': build/'lvgallery.fxe'}, files)
    finally:
        if old_kernel is None:
            os.environ.pop('FUSE_X_TEST_KERNEL', None)
        else:
            os.environ['FUSE_X_TEST_KERNEL'] = old_kernel
    g = x.g
    trace_at = 0
    paste, boundary, paste_seconds = '', '', 0

    def launch(*paths):
        nonlocal trace_at
        at = len(g.log)
        trace_at = at
        g.shell('lvedit --trace '+' '.join(map(shlex.quote, paths))+' &')
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
        scale = next(window['scale'] for window in x.windows() if window['id'] == wid)
        x.frame(wid, width=880, height=600, scale=scale)
        x.send(wid, action=4)
        x.send(wid, action=2, clipboard=path)
        x.key(wid, 40)

    def button(wid, caption, at):
        pattern = rb'EDIT_RECT '+re.escape(caption.encode())+rb' (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)'
        values = x.wait(lambda: re.findall(pattern, g.log[at:]))[-1]
        px, py, width, height, density = map(int, values)
        x.click(wid, (px+width//2)//density, (py+height//2)//density)

    def rects(at=None):
        result = {}
        for name, px, py, width, height, density in re.findall(
                rb'EDIT_RECT (\S+) (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)', g.log[trace_at if at is None else at:]):
            result[name.decode()] = dict(rect=list(map(int, (px, py, width, height))), density=int(density))
        return result

    def tab_state():
        text = bytes(g.log[trace_at:]).decode(errors='replace')
        headers = list(re.finditer(r'EDIT_TABS count=(\d+) active=(\d+)', text))
        if not headers:
            return None
        last = headers[-1]
        count, active = map(int, last.groups())
        documents = {}
        for line in text[last.end():].splitlines():
            if line.startswith('EDIT_DOCUMENT ') and ' name=' in line:
                fields, name = line.split(' name=', 1)
                values = dict(re.findall(r'(\w+)=(\S+)', fields))
                values['name'] = name
                documents[int(values['index'])] = values
        return dict(count=count, active=active, documents=documents) if len(documents) == count else None

    def tabs_are(count, active=None):
        def expected():
            state = tab_state()
            if not state or state['count'] != count:
                return None
            return state if active is None or state['documents'][state['active']]['name'] == active else None
        return x.wait(expected)

    def click_rect(wid, rect):
        px, py, width, height = rect['rect']
        scale = rect['density']
        x.click(wid, (px+width//2)//scale, (py+height//2)//scale)

    def motion_rect(wid, rect, right=False):
        px, py, width, height = rect['rect']
        scale = rect['density']
        x.send(wid, input=dict(type='motion', x=(px+width-8 if right else px+width//2)//scale,
                              y=(py+height//2)//scale))

    def refresh_tabs(wid):
        # Pointer motion does not change editing focus. A caret move asks the
        # app to publish fresh tab geometry without changing any document text.
        at = len(g.log)
        x.key(wid, 79)
        x.wait(lambda: b'EDIT_TABS ' in g.log[at:])
        x.pause(.1)
        return rects()

    def select_tab(wid, index, count, name):
        motion_rect(wid, rects()[f'tab-{index}'], right=True)
        px, py, width, height = rects()[f'tab-{index}']['rect']
        scale = rects()[f'tab-{index}']['density']
        x.click(wid, (px+width-8)//scale, (py+height//2)//scale)
        return tabs_are(count, name)

    def close_tab(wid, index):
        motion_rect(wid, rects()[f'tab-{index}'], right=True)
        x.pause(.1)
        motion_rect(wid, rects()[f'tab-close-{index}'])
        x.pause(.1)
        click_rect(wid, rects()[f'tab-close-{index}'])

    def end_editor(wid):
        x.send(wid, close=True)
        x.wait(lambda: not x.window('Text Editor'))
        assert 'EDIT_STATUS:0' in g.shell('wait $editor; echo EDIT_STATUS:$?')

    def ink_pixels(image, rect):
        px, py, width, height = rect['rect']
        return sum(max(image[2][(y*image[0]+p)*3:(y*image[0]+p)*3+3]) > 120
                   for y in range(py, py+height) for p in range(px, px+width))

    def single_line_caption(image, row, close, scale):
        # Read the actual rendered caption, excluding the left close slot,
        # right dirty-marker slot and tab border. A wrapped label previously
        # clipped its first line at the top and drew a second 'txt' below it.
        left = close[0]+close[2]+6*scale
        right = row[0]+row[2]-20*scale
        ink = [(px, py) for py in range(row[1], row[1]+row[3])
               for px in range(left, right)
               if max(image[2][(py*image[0]+px)*3:(py*image[0]+px)*3+3]) > 120]
        assert ink, (row, close)
        top, bottom = min(py for _, py in ink), max(py for _, py in ink)
        assert top >= row[1]+3*scale and bottom < row[1]+row[3]-3*scale, (scale, row, top, bottom)
        assert bottom-top+1 <= 22*scale, (scale, row, top, bottom)
        return dict(ink_top=top-row[1], ink_height=bottom-top+1, tab_height=row[3])

    def editor_layout(wid, scale):
        current = rects()
        assert 'toolbar' not in current and 'path' not in current and 'path-label' not in current
        strip = current['tabs']['rect']
        area = current['document-area']['rect']
        surface = current['editor']['rect']
        menubar = current['menubar']['rect']
        assert strip[:2] == area[:2], (strip, area)
        assert strip[0] == surface[0] and strip[2] == surface[2]
        assert strip[1]+strip[3] == surface[1], (strip, surface)
        assert strip[3] == 32*scale, strip
        assert 0 <= area[1]-menubar[1]-menubar[3] <= 12*scale
        assert {'File', 'Edit', 'View', 'Help'} <= current.keys()
        return dict(menubar=menubar, tabs=strip, editor=surface, document_area=area)

    def tab_interactions():
        layouts = {}
        for scale in (1, 2):
            paths = tab_paths[scale]
            alpha, beta, gamma = map(lambda path: Path(path).name, paths)
            wid = launch(*paths)
            x.send(wid, density=scale)
            x.send(wid, input=dict(type='motion', x=400, y=400))
            tabs_are(3, gamma)
            before = refresh_tabs(wid)
            layouts[str(scale)] = editor_layout(wid, scale)
            rest = gui.ppm(x.snapshot(wid, f'tabs-rest-{scale}x', width=880, height=600, scale=scale))
            widths = {name: value['rect'][2] for name, value in before.items() if re.fullmatch(r'tab-\d+', name)}
            for index in range(1, 4):
                assert ink_pixels(rest, before[f'tab-close-{index}']) == 0, (scale, index)
            motion_rect(wid, before['tab-2'], right=True)
            hovered = refresh_tabs(wid)
            image = gui.ppm(x.snapshot(wid, f'tabs-hover-background-{scale}x', width=880, height=600, scale=scale))
            assert ink_pixels(image, hovered['tab-close-2']) >= 4*scale*scale
            assert widths == {name: value['rect'][2] for name, value in hovered.items() if re.fullmatch(r'tab-\d+', name)}
            row, close = hovered['tab-2']['rect'], hovered['tab-close-2']['rect']
            assert row[0] <= close[0] < row[0]+row[2]//2 and close[0]+close[2] <= row[0]+row[2], (row, close)
            motion_rect(wid, hovered['tab-close-2'])
            child = refresh_tabs(wid)
            image = gui.ppm(x.snapshot(wid, f'tabs-close-child-hover-{scale}x', width=880, height=600, scale=scale))
            assert ink_pixels(image, child['tab-close-2']) >= 4*scale*scale
            assert widths == {name: value['rect'][2] for name, value in child.items() if re.fullmatch(r'tab-\d+', name)}
            click_rect(wid, child['tab-close-2'])
            state = tabs_are(2, gamma)
            assert beta not in [d['name'] for d in state['documents'].values()]

            select_tab(wid, 1, 2, alpha)
            marker = f'Unsaved alpha contents at {scale}x.\n'
            clean_width = rects()['tab-1']['rect'][2]
            changed(wid, marker)
            tabs_are(2, alpha)
            assert rects()['tab-1']['rect'][2] == clean_width, 'Dirty dot changed the tab width'
            # Named documents now autosave on blur. An external modification
            # deliberately keeps this buffer dirty without overwriting disk,
            # so the close/cancel/discard safeguards remain exercised.
            external = f'External alpha before discard at {scale}x.'
            g.shell('echo '+shlex.quote(external)+' > '+shlex.quote(paths[0]))
            at = len(g.log)
            select_tab(wid, 2, 2, gamma)
            x.logged('EDIT_AUTOSAVE_FAILED '+paths[0]+' ', at=at)
            at = len(g.log)
            close_tab(wid, 2)  # Active clean close selects the dirty neighbour.
            x.logged('EDIT_CLOSE count=1', at=at)
            state = tabs_are(1, alpha)
            assert state['documents'][1]['dirty'] == '1'
            assert int(state['documents'][1]['bytes']) == len(marker.encode())
            at = len(g.log)
            close_tab(wid, 1)
            x.logged('EDIT_DIALOG unsaved', at=at)
            button(wid, 'Cancel', at)
            tabs_are(1, alpha)
            at = len(g.log)
            close_tab(wid, 1)
            x.logged('EDIT_DIALOG unsaved', at=at)
            x.snapshot(wid, f'tabs-dirty-active-close-{scale}x', width=880, height=600, scale=scale)
            button(wid, 'Discard', at)
            state = x.wait(lambda: tab_state() if tab_state() and tab_state()['count'] == 1 and
                           tab_state()['documents'][1]['name'] != alpha else None)
            assert state['documents'][1]['dirty'] == '0'
            disk = g.shell('cat '+shlex.quote(paths[0]))
            assert external in disk and marker not in disk
            end_editor(wid)

            wid = launch(paths[0], paths[2])
            x.send(wid, density=scale)
            tabs_are(2, gamma)
            select_tab(wid, 1, 2, alpha)
            changed(wid, marker)
            external = f'External alpha before save at {scale}x.'
            g.shell('echo '+shlex.quote(external)+' > '+shlex.quote(paths[0]))
            at = len(g.log)
            select_tab(wid, 2, 2, gamma)
            x.logged('EDIT_AUTOSAVE_FAILED '+paths[0]+' ', at=at)
            at = len(g.log)
            close_tab(wid, 1)
            x.logged('EDIT_DIALOG unsaved', at=at)
            button(wid, 'Cancel', at)
            state = tabs_are(2, gamma)
            assert state['documents'][1]['dirty'] == '1'
            at = len(g.log)
            close_tab(wid, 1)
            x.logged('EDIT_DIALOG unsaved', at=at)
            x.snapshot(wid, f'tabs-dirty-background-close-{scale}x', width=880, height=600, scale=scale)
            button(wid, 'Save', at)
            x.logged('EDIT_DIALOG conflict', at=at)
            button(wid, 'Overwrite', at)
            x.logged('EDIT_SAVE '+paths[0], at=at)
            x.logged('EDIT_CLOSE count=1', at=at)
            state = tabs_are(1, gamma)
            assert state['documents'][1]['dirty'] == '0'
            disk = g.shell('cat '+shlex.quote(paths[0]))
            assert marker in disk and external not in disk
            end_editor(wid)

        overflow = {}
        for scale in (1, 2):
            # Keep the fixture launch below the guest TTY's command-line cap;
            # subsequent files use the same real Open workflow as an end user.
            wid = launch(long_paths[0])
            x.send(wid, density=scale)
            for index, path in enumerate(long_paths[1:], 2):
                at = len(g.log)
                x.ctrl(wid, 18)
                x.logged('EDIT_DIALOG open', at=at)
                path_dialog(wid, path)
                x.logged('EDIT_OPEN '+path, at=at)
                tabs_are(index, Path(path).name)
            x.send(wid, input=dict(type='motion', x=400, y=400))
            tabs_are(len(long_paths), Path(long_paths[-1]).name)
            current = refresh_tabs(wid)
            strip, last = current['tabs']['rect'], current[f'tab-{len(long_paths)}']['rect']
            assert strip[0] <= last[0] and last[0]+last[2] <= strip[0]+strip[2], (strip, last)
            assert current['tab-1']['rect'][0] < strip[0], current
            image = gui.ppm(x.snapshot(wid, f'tabs-overflow-last-unicode-{scale}x', width=880, height=600, scale=scale))
            unicode_caption = single_line_caption(image, last, current[f'tab-close-{len(long_paths)}']['rect'], scale)
            x.ctrl(wid, 43)
            tabs_are(len(long_paths), Path(long_paths[0]).name)
            current = refresh_tabs(wid)
            first = current['tab-1']['rect']
            assert strip[0] <= first[0] and first[0]+first[2] <= strip[0]+strip[2], (strip, first)
            image = gui.ppm(x.snapshot(wid, f'tabs-overflow-first-{scale}x', width=880, height=600, scale=scale))
            ascii_caption = single_line_caption(image, first, current['tab-close-1']['rect'], scale)
            x.ctrl(wid, 43, shift=True)
            tabs_are(len(long_paths), Path(long_paths[-1]).name)
            overflow[str(scale)] = dict(first=first, last=last, strip=strip,
                                       ascii_caption=ascii_caption, unicode_caption=unicode_caption)
            end_editor(wid)
        return dict(layouts=layouts, overflow=overflow)

    def choose_language(wid, language):
        at = len(g.log)
        click_rect(wid, rects()['View'])
        x.key(wid, 74)  # View menu: Wrap, Syntax language, Next, Previous.
        x.key(wid, 81)
        x.key(wid, 40)
        x.logged('EDIT_LANGUAGE_MENU ', at=at)
        x.key(wid, 74)
        for _ in range(language):
            x.key(wid, 81)
        scale = next(window['scale'] for window in x.windows() if window['id'] == wid)
        x.snapshot(wid, f'syntax-chooser-{language}-{scale}x', width=880, height=600, scale=scale)
        x.key(wid, 40)
        x.logged(f'EDIT_LANGUAGE choice={language} ', at=at)
        return x.wait(lambda: tab_state() if tab_state() and
                      int(tab_state()['documents'][tab_state()['active']]['choice']) == language else None)

    def editor_crop(wid, scale):
        image = x.frame(wid, width=880, height=600, scale=scale)
        px, py, width, height = rects()['editor']['rect']
        rows = b''.join(image[2][((py+y)*image[0]+px)*3:((py+y)*image[0]+px+width)*3]
                        for y in range(height))
        return width, height, rows

    def chromatic(image):
        return sum(max(image[2][i:i+3]) > 100 and max(image[2][i:i+3])-min(image[2][i:i+3]) > 40
                   for i in range(0, len(image[2]), 3))

    def syntax_interactions():
        cases = {}
        for scale in (1, 2):
            wid = launch()
            x.send(wid, density=scale)
            initial = tabs_are(1)
            js_name = initial['documents'][1]['name']
            javascript = 'const answer = 42;\nconsole.log(answer);\n'
            changed(wid, javascript)
            plain_js = editor_crop(wid, scale)
            state = choose_language(wid, 23)  # JavaScript on an unsaved tab.
            assert state['documents'][1]['dirty'] == '1' and int(state['documents'][1]['effective']) == 23
            highlighted_js = editor_crop(wid, scale)
            assert chromatic(highlighted_js) > chromatic(plain_js)+10*scale*scale
            x.snapshot(wid, f'syntax-unsaved-javascript-{scale}x', width=880, height=600, scale=scale)
            at = len(g.log)
            x.ctrl(wid, 29)
            x.logged('EDIT_CHANGE bytes=0 dirty=0', at=at)
            assert tabs_are(1)['documents'][1]['choice'] == '23'
            at = len(g.log)
            x.ctrl(wid, 29, shift=True)
            x.logged(f'EDIT_CHANGE bytes={len(javascript.encode())} dirty=1', at=at)

            x.ctrl(wid, 17)
            rfl = 'fn main() -> i16 {\n    let value = 42;\n    value\n}\n'
            changed(wid, rfl)
            rfl_name = tabs_are(2)['documents'][2]['name']
            plain_rfl = editor_crop(wid, scale)
            state = choose_language(wid, 4)
            highlighted_rfl = editor_crop(wid, scale)
            assert state['documents'][1]['choice'] == '23' and state['documents'][2]['choice'] == '4'
            assert chromatic(highlighted_rfl) > chromatic(plain_rfl)+10*scale*scale
            x.snapshot(wid, f'syntax-unsaved-rfl-{scale}x', width=880, height=600, scale=scale)
            x.ctrl(wid, 43)
            state = tabs_are(2, js_name)
            assert state['documents'][1]['choice'] == '23' and state['documents'][2]['choice'] == '4'

            saved_text, saved_js = f'/editor-tabs/syntax-{scale}.txt', f'/editor-tabs/syntax-{scale}.js'
            at = len(g.log)
            x.ctrl(wid, 22, shift=True)
            x.logged('EDIT_DIALOG save', at=at)
            path_dialog(wid, saved_text)
            x.logged('EDIT_SAVE '+saved_text, at=at)
            state = tabs_are(2, Path(saved_text).name)
            assert state['documents'][1]['choice'] == '23' and state['documents'][1]['dirty'] == '0'
            at = len(g.log)
            x.key(wid, 62)
            x.logged('EDIT_RELOAD', at=at)
            state = tabs_are(2, Path(saved_text).name)
            assert state['documents'][1]['choice'] == '23' and state['documents'][1]['effective'] == '23'
            state = choose_language(wid, 0)
            assert state['documents'][1]['effective'] == '1' and state['documents'][1]['dirty'] == '0'
            auto_plain = editor_crop(wid, scale)
            assert chromatic(auto_plain) < chromatic(highlighted_js)-10*scale*scale
            at = len(g.log)
            x.ctrl(wid, 22, shift=True)
            x.logged('EDIT_DIALOG save', at=at)
            path_dialog(wid, saved_js)
            x.logged('EDIT_SAVE '+saved_js, at=at)
            state = tabs_are(2, Path(saved_js).name)
            assert state['documents'][1]['choice'] == '0' and state['documents'][1]['effective'] == '23'
            assert state['documents'][2]['choice'] == '4'
            assert javascript in g.shell('cat '+shlex.quote(saved_js))
            x.snapshot(wid, f'syntax-automatic-save-as-js-{scale}x', width=880, height=600, scale=scale)
            at = len(g.log)
            x.send(wid, close=True)
            x.logged('EDIT_DIALOG unsaved operation=2 name='+rfl_name, at=at)
            button(wid, 'Discard', at)
            x.wait(lambda: not x.window('Text Editor'))
            assert 'EDIT_STATUS:0' in g.shell('wait $editor; echo EDIT_STATUS:$?')
            cases[str(scale)] = dict(javascript_extra_coloured_pixels=chromatic(highlighted_js)-chromatic(plain_js),
                                    rfl_extra_coloured_pixels=chromatic(highlighted_rfl)-chromatic(plain_rfl))
        return cases

    try:
        if not args.new_only:
            wid = launch(str(sample))
            for scale in (1, 2):
                x.send(wid, density=scale)
                x.send(wid, input=dict(type='motion', x=0, y=0))
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
        tab_acceptance = tab_interactions()
        print('EDITOR_GUEST integrated tabs, hover/close, dirty-document isolation and overflow passed at 1x/2x', flush=True)
        syntax_acceptance = syntax_interactions()
        print('EDITOR_GUEST per-tab syntax, unsaved JS/RFL, undo and Auto Save As/reload passed at 1x/2x', flush=True)
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
            x.send(gallery, input=dict(type='motion', x=0, y=0))
            x.frame(gallery, width=760, height=560, scale=scale)
            expected = gui.ppm(references/f'gallery-editor-{scale}x.ppm')
            x.snapshot(gallery, f'gallery-editor-{scale}x', width=760, height=560, scale=scale)
            x.wait(lambda: gui.ppm(x.frames/f'{gallery}.ppm') == expected)
        x.send(gallery, density=1)
        x.frame(gallery, width=760, height=560)
        gallery_rects = re.findall(rb'GALLERY_RECT editor (\d+) (\d+) (\d+) (\d+)', g.log[at:])
        left, top, right, bottom = map(int, gallery_rects[-1])
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
                      kernel_sha256=hashlib.sha256(args.kernel.resolve().read_bytes()).hexdigest() if args.kernel else None,
                      runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      scope='new workflows' if args.new_only else 'full',
                      tabs=tab_acceptance,
                      syntax=syntax_acceptance,
                      paste_octets=len(paste.encode()), paste_seconds=round(paste_seconds, 3),
                      clipboard_boundary_octets=len(boundary.encode()),
                      checks=['exact host/guest editor pixels at 1x/2x', 'large Unicode paste and grouped undo/redo',
                              'exact 32768-octet clipboard paste/copy/cut/undo round trip',
                              'atomic saves and Unicode filenames', 'independent tabs and HOME recents',
                              'failed binary open preserves document',
                              'shared same-window directory browser, keyboard file selection and small-window layout', 'resize and density transitions',
                              'idle publication', 'restart and native close cancel/discard',
                              'integrated left close appears on tab/child hover with stable widths at 1x/2x',
                              'active/background close preserves dirty cancel/discard/save decisions after an autosave conflict',
                              'long ASCII/Unicode captions remain one unclipped line; keyboard wrap scrolls active tabs into view',
                              'main menubar, attached tabs/text and status without toolbar or full path row',
                              'unsaved per-tab JS/RFL syntax changes repaint immediately without extra undo entries',
                              'explicit syntax survives Save As/reload and Auto resumes filename detection',
                              'gallery Editor pixel parity, typing and page teardown/recreation'])
        if args.new_only:
            report['checks'] = report['checks'][10:]
            report.pop('paste_octets');report.pop('paste_seconds');report.pop('clipboard_boundary_octets')
    finally:
        x.close()
    # Guest.close already shuts down/syncs and checks the disposable root.
    # Publish success only after that existing integrity check has succeeded.
    report.update(private_images_only=True, networking='off', private_fsck='passed')
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Graphical editor: input, files, tabs, syntax, paste, density and close workflows passed', flush=True)


if __name__ == '__main__':
    main()
