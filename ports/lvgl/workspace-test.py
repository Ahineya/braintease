#!/usr/bin/env python3
"""Shared commands and split panes through real Fuse-X input in an offline image."""
from pathlib import Path
import hashlib, importlib.util, json, re, shlex

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
BUILD = ROOT / 'build/lvgl'
spec = importlib.util.spec_from_file_location('gui', PORT / 'gui-test.py')
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def main():
    out = BUILD / 'workspace-tests'
    out.mkdir(exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    sample = PORT / 'examples/measurements.csv'
    apps = {name: BUILD / (name + '.fxe') for name in ('lvgallery', 'lvdata')}
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in apps.items()}
    x = gui.GUI(out, apps, [dict(name=str(sample), path=str(sample), mode=0o100644)])
    g = x.g

    def drag(wid, start, end):
        x.send(wid, input=dict(type='button', button=0, down=True, x=start[0], y=start[1]))
        x.send(wid, input=dict(type='motion', x=end[0], y=end[1]))
        x.send(wid, input=dict(type='button', button=0, down=False, x=end[0], y=end[1]))

    def equal_frame(wid, path, scale, width, height):
        expected = gui.ppm(path)
        x.frame(wid, scale=scale, width=width, height=height)
        x.wait(lambda: gui.ppm(x.frames / f'{wid}.ppm') == expected)

    try:
        g.shell('lvgallery --trace &'); g.shell('gallery=$!')
        wid = x.wait(lambda: x.window('Graphite Gallery'))['id']
        at = len(g.log); x.key(wid, 63); x.logged('GALLERY_PAGE 5', at)
        for scale in (1, 2):
            x.send(wid, density=scale)
            equal_frame(wid, BUILD / 'host' / f'gallery-workspace-{scale}x.ppm', scale, 760, 560)
            x.snapshot(wid, f'workspace-{scale}x', scale=scale, width=760)
        x.send(wid, density=1); x.frame(wid, width=760)
        at = len(g.log); x.ctrl(wid, 21); x.logged('GALLERY_ACTION 1 runs=1', at)
        at = len(g.log); x.click(wid, 220, 275); x.logged('GALLERY_ACTION 1 runs=2', at)
        # Both Run buttons, the menu and the shortcut use the same disabled state.
        at = len(g.log); x.click(wid, 184, 128); x.logged('GALLERY_ACTION_ENABLED 0', at)
        at = len(g.log)
        x.ctrl(wid, 21); x.click(wid, 210, 95); x.click(wid, 220, 275); x.pause()
        assert b'GALLERY_ACTION 1 ' not in g.log[at:]
        at = len(g.log); x.click(wid, 390, 95); x.logged('GALLERY_MENU workspace', at)
        # Disabled Run is skipped: the first keyboard action is Reset.
        at = len(g.log); x.key(wid, 40); x.logged('GALLERY_ACTION 2 runs=0', at)
        at = len(g.log); x.click(wid, 184, 128); x.logged('GALLERY_ACTION_ENABLED 1', at)
        at = len(g.log); x.click(wid, 390, 95); x.logged('GALLERY_MENU workspace', at)
        at = len(g.log); x.key(wid, 40); x.logged('GALLERY_ACTION 1 runs=1', at)
        # Modal overlays own shortcuts and page navigation.
        x.ctrl(wid, 12); x.frame(wid, width=760)
        at = len(g.log); x.ctrl(wid, 21); x.ctrl(wid, 7); x.key(wid, 58); x.pause()
        assert not any(s in g.log[at:] for s in (b'GALLERY_ACTION ', b'GALLERY_PAGE ', b'GALLERY_SPLIT '))
        x.key(wid, 41)
        # Pointer drag in both axes, followed by keyboard adjustment of the focused divider.
        at = len(g.log); drag(wid, (398, 300), (440, 300)); x.logged('GALLERY_SPLIT ', at)
        positions = x.wait(lambda: re.findall(rb'GALLERY_SPLIT (\d+) (\d+) visible=(\d+)\r?\n', g.log[at:]))
        position = int(positions[-1][0]); assert position > 400
        at = len(g.log); x.key(wid, 79); x.logged(f'GALLERY_SPLIT {position + 25} ', at)
        at = len(g.log); x.ctrl(wid, 42); x.logged('GALLERY_ACTION 2 runs=0', at)
        x.frame(wid, width=760)
        at = len(g.log); drag(wid, (600, 373), (600, 340)); x.logged('GALLERY_SPLIT ', at)
        positions = x.wait(lambda: re.findall(rb'GALLERY_SPLIT (\d+) (\d+) visible=(\d+)\r?\n', g.log[at:]))
        vertical = int(positions[-1][1]); assert vertical < 600
        at = len(g.log); x.key(wid, 81); x.logged(f'GALLERY_SPLIT 400 {vertical + 25} ', at)
        at = len(g.log); x.ctrl(wid, 7); x.logged('visible=0', at)
        at = len(g.log); x.ctrl(wid, 21); x.logged('GALLERY_ACTION 1 runs=1', at)
        at = len(g.log); x.ctrl(wid, 7); x.logged('visible=1', at)
        # Resizing/density must preserve contents and the preferred divider positions.
        at = len(g.log)
        for width, height, scale in ((640, 460, 1), (1470, 900, 2), (760, 560, 1)):
            x.send(wid, density=scale); x.send(wid, resize=[width, height])
            x.frame(wid, width=width, height=height, scale=scale)
        assert b'GALLERY_SPLIT ' not in g.log[at:]
        at = len(g.log); x.ctrl(wid, 21); x.logged('GALLERY_ACTION 1 runs=2', at)
        # Page replacement releases bindings and nested panes, then rebuilds cleanly.
        for key in (58, 63, 62, 63): x.key(wid, key)
        at = len(g.log); x.ctrl(wid, 21); x.logged('GALLERY_ACTION 1 runs=1', at)
        x.send(wid, input=dict(type='motion', x=750, y=550)); x.frame(wid, width=760)
        stamp = (x.frames / f'{wid}.ppm').stat().st_mtime_ns
        x.pause(1.1); assert (x.frames / f'{wid}.ppm').stat().st_mtime_ns == stamp
        x.send(wid, close=True); x.wait(lambda: not x.window('Graphite Gallery'))
        assert 'STATUS:0' in g.shell('wait $gallery; echo STATUS:$?')

        def launch():
            at = len(g.log)
            g.shell('lvdata --trace ' + shlex.quote(str(sample)) + ' &'); g.shell('viewer=$!')
            wid = x.wait(lambda: x.window('Data Viewer'))['id']; x.logged('DATA_OPEN ', at)
            return wid

        def close(wid):
            x.ctrl(wid, 26); x.wait(lambda: not x.window('Data Viewer'))
            assert 'STATUS:0' in g.shell('wait $viewer; echo STATUS:$?')

        wid = launch()
        at = len(g.log); x.ctrl(wid, 7); x.logged('DATA_SPLIT 700 visible=1', at)
        for scale in (1, 2):
            x.send(wid, density=scale)
            equal_frame(wid, BUILD / 'host' / f'data-details-{scale}x.ppm', scale, 800, 560)
            x.snapshot(wid, f'details-{scale}x', scale=scale)
        x.send(wid, density=1); x.frame(wid)
        at = len(g.log); drag(wid, (553, 300), (500, 300)); x.logged('DATA_SPLIT ', at)
        matches = x.wait(lambda: re.findall(rb'DATA_SPLIT (\d+) visible=1\r?\n', g.log[at:]))
        saved = int(matches[-1]); assert saved < 700
        at = len(g.log); x.click(wid, 100, 193); x.logged('DATA_SELECT 1 0', at)
        x.snapshot(wid, 'details-selected')
        close(wid)
        settings = g.shell('cat /data/home/.lvdata')
        assert f'split={saved}' in settings and 'details=1' in settings
        wid = launch()
        at = len(g.log); x.ctrl(wid, 7); x.logged(f'DATA_SPLIT {saved} visible=0', at)
        at = len(g.log); x.ctrl(wid, 7); x.logged(f'DATA_SPLIT {saved} visible=1', at)
        close(wid)
        assert not any(s in g.log for s in (b'native runtime error', b'panic:', b'graphics operation failed'))
        report = dict(status='passed', executables=hashes, checks=[
            'exact host/guest Workspace and details rendering at 1x/2x',
            'one enabled state for duplicate buttons, menus and shortcuts',
            'disabled menu traversal and modal shortcut isolation',
            'pointer and keyboard split adjustment in both axes',
            'collapse/restore, density and viewport preservation',
            'page replacement and idle publication',
            'Data Viewer cell inspection and HOME settings across restart',
            'normal close and cleanup'])
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Workspace: shared commands, split panes and persistent Data Viewer details passed')
    finally:
        x.close()


if __name__ == '__main__': main()
