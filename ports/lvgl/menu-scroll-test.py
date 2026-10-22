#!/usr/bin/env python3
"""Exercise shared Graphite dropdown scrolling in isolated offline guests."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('desktop_test', ROOT/'fuse-x/test_desktop_mode.py')
desktop_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop_test)


class MenuGUI(desktop_test.DesktopGUI):
    def __init__(self, args, nested):
        self.out = args.output/('desktop' if nested else 'native')
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='graphite-menu-scroll-')
        work = Path(self.temp.name)
        self.frames, self.mailbox = work/'frames', work/'event.json'
        self.nested = nested
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(args.kernel), FUSE_X_DESKTOP='1' if nested else '0',
                          FUSE_X_DESKTOP_SIZE='1280x800', FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), RCC_NATIVE_AUDIO='off')
        try:
            self.g = desktop_test.Guest(work, 'native',
                {name: args.build/(name+'.fxe') for name in ('lvedit', 'lvgallery')},
                [dict(name='/dev/gfx', mode=0o20600, device=0x400),
                 dict(name='/data/home', directory=True, mode=0o40755)],
                blocks=131072, build_override=args.guest_build)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell('HOME=/data/home; export HOME; unset LVGL_SCALE')

    def target(self, wid):
        return 0 if self.nested else wid

    def pointer(self, wid, px, py):
        if self.nested:
            window = self.wait(lambda: self.internal(wid))
            px, py = px+window['x'], py+window['y']
        self.send(self.target(wid), input=dict(type='motion', x=px, y=py))

    def local_click(self, wid, px, py):
        if self.nested:
            window = self.wait(lambda: self.internal(wid))
            px, py = px+window['x'], py+window['y']
        self.click(self.target(wid), px, py)

    def density(self, wid, scale):
        self.send(self.target(wid), density=scale)

    def wheel(self, wid, delta):
        self.send(self.target(wid), input=dict(type='wheel', x=0, y=delta))

    def capture(self, wid, name, width, height, scale):
        image = self.frame(wid, width=width, height=height, scale=scale)
        shutil.copyfile(self.frames/f'{wid}.ppm', self.out/(name+'.ppm'))
        desktop_test.save_png(self.out/(name+'.png'), image)
        return image


def crop(image, rectangle):
    x, y, width, height = rectangle
    return b''.join(image[2][((y+row)*image[0]+x)*3:((y+row)*image[0]+x+width)*3]
                    for row in range(height))


def text_mask(image, rectangle, scale):
    x, y, width, height = rectangle
    # Ignore borders, scrollbar and hover fills. Text and icons retain their
    # neutral bright pixels when the dropdown moves under the same pointer.
    result = set()
    for py in range(8*scale, height-8*scale):
        for px in range(8*scale, width-12*scale):
            at = ((y+py)*image[0]+x+px)*3
            pixel = image[2][at:at+3]
            if min(pixel) > 110 and max(pixel)-min(pixel) < 60:
                result.add((px, py))
    return result


def shifted_fraction(before, after, distance):
    moved = {(x, y-distance) for x, y in before if y >= distance+16}
    # Compare the common interior, excluding newly revealed bottom rows.
    moved = {point for point in moved if point in after}
    return len(moved)/max(1, len({point for point in before if point[1] >= distance+16}))


def exercise(args, nested):
    ui = MenuGUI(args, nested)
    cases, failed = [], []
    try:
        for app, title, width, height in (('lvedit', 'Text Editor', 880, 600),
                                          ('lvgallery', 'Graphite Gallery', 760, 560)):
            at = len(ui.g.log)
            ui.g.shell(app+' --trace &')
            ui.g.shell('app=$!')
            wid = ui.wait(lambda: ui.window(title))['id']
            if nested:
                ui.wait(lambda: ui.desktop().get('focused') == wid)
            for scale in (1, 2):
                ui.density(wid, scale)
                ui.frame(wid, width=width, height=height, scale=scale)
                if app == 'lvedit':
                    def open_menu():
                        start = len(ui.g.log)
                        rectangles = re.findall(rb'EDIT_RECT View (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)', ui.g.log[at:])
                        x, y, w, h, density = map(int, rectangles[-1])
                        ui.local_click(wid, (x+w//2)//density, (y+h//2)//density)
                        ui.key(ui.target(wid), 74)
                        ui.key(ui.target(wid), 81)
                        ui.key(ui.target(wid), 40)
                        ui.logged('EDIT_LANGUAGE_MENU ', start)
                        rect = re.findall(rb'EDIT_RECT syntax-menu (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)', ui.g.log[start:])[-1]
                        return tuple(map(int, rect[:4]))
                else:
                    start = len(ui.g.log)
                    ui.key(ui.target(wid), 65)
                    ui.logged('GALLERY_FORMS_READY choices=65', start)
                    ui.logged('GALLERY_PAGE 7', start)
                    ui.frame(wid, width=width, height=height, scale=scale)
                    def open_menu():
                        controls = ui.wait(lambda: re.findall(
                            rb'GALLERY_RECT forms-choice (-?\d+) (-?\d+) (-?\d+) (-?\d+)', ui.g.log[at:]))
                        x, y, right, bottom = map(int, controls[-1])
                        ui.pointer(wid, (x+right)//2, (y+bottom)//2)
                        ui.pause(.1)
                        opened_at = len(ui.g.log)
                        ui.local_click(wid, (x+right)//2, (y+bottom)//2)
                        rectangle = (min(x, width-320)*scale, 16*scale, 320*scale, (height-16)*scale)
                        # Building a 65-row menu can outlive input delivery.
                        # Wait for its first row's actual painted text, rather
                        # than mistaking the previous Forms frame for a menu.
                        def painted():
                            image = desktop_test.ppm(ui.frames/f'{wid}.ppm')
                            if not image or image[:2] != (width*scale, height*scale):
                                return False
                            rx, ry, _, _ = rectangle
                            count = 0
                            for py in range(ry+14*scale, ry+30*scale):
                                for px in range(rx+36*scale, rx+130*scale):
                                    at = (py*image[0]+px)*3
                                    if min(image[2][at:at+3]) > 110:
                                        count += 1
                            return count > 30*scale*scale
                        ui.wait(painted)
                        assert b'GALLERY_PATH_OPEN ' not in ui.g.log[opened_at:], 'Forms fixture opened a file picker'
                        return rectangle
                rectangle = open_menu()
                rx, ry, rw, rh = rectangle
                label = ((rx+60*scale)//scale, (ry+rh//2)//scale)
                padding = ((rx+rw-24*scale)//scale, (ry+rh//2)//scale)
                name = f'{app}-{scale}x'
                measurements = {}
                for hit, point, deltas in (('label', label, [-65536]),
                                            ('row', padding, [-8192]*8)):
                    ui.pointer(wid, *point)
                    before = ui.capture(wid, name+'-'+hit+'-before', width, height, scale)
                    for delta in deltas:
                        ui.wheel(wid, delta)
                    after = ui.capture(wid, name+'-'+hit+'-down', width, height, scale)
                    fraction = shifted_fraction(text_mask(before, rectangle, scale),
                                                text_mask(after, rectangle, scale), 40*scale)
                    moved = fraction > .75
                    ui.wheel(wid, 65536)
                    restored = ui.capture(wid, name+'-'+hit+'-up', width, height, scale)
                    returned = text_mask(restored, rectangle, scale) == text_mask(before, rectangle, scale)
                    measurements[hit] = dict(shifted_text_fraction=round(fraction, 4),
                                             scrolled=moved, reverse_restores=returned)
                    if not moved or not returned:
                        failed.append(f'{name}: {hit} wheel movement/restoration')
                # A modal backdrop must block wheel delivery to the document
                # or gallery below it, even outside the dropdown rectangle.
                ui.pointer(wid, width-30, height-90)
                before = ui.frame(wid, width=width, height=height, scale=scale)
                ui.wheel(wid, -65536)
                after = ui.frame(wid, width=width, height=height, scale=scale)
                blocked = before == after
                if not blocked:
                    failed.append(f'{name}: backdrop leaked wheel')
                ui.key(ui.target(wid), 41)
                rectangle = open_menu()  # Recreated menu must accept wheel too.
                ui.pointer(wid, *label)
                before = ui.frame(wid, width=width, height=height, scale=scale)
                ui.wheel(wid, -65536)
                after = ui.frame(wid, width=width, height=height, scale=scale)
                reopened = shifted_fraction(text_mask(before, rectangle, scale),
                                              text_mask(after, rectangle, scale), 40*scale) > .75
                if not reopened:
                    failed.append(f'{name}: reopened dropdown wheel')
                start = len(ui.g.log)
                ui.key(ui.target(wid), 74)
                ui.key(ui.target(wid), 81)
                ui.pause(.1)
                assert (b'EDIT_LANGUAGE choice=' if app == 'lvedit' else b'GALLERY_CHOICE ') not in ui.g.log[start:], \
                    'Dropdown navigation committed before Enter (the menu was not open)'
                ui.key(ui.target(wid), 40)
                ui.logged('EDIT_LANGUAGE choice=1 ' if app == 'lvedit' else 'GALLERY_CHOICE 1', start)
                cases.append(dict(app=app, scale=scale, wheel=measurements,
                                  outside_overlay_blocked=blocked, reopened_scrolls=reopened,
                                  keyboard_selection='passed'))
                print(f'MENU_SCROLL {"desktop" if nested else "native"} {name}: '
                      f'{json.dumps(cases[-1], sort_keys=True)}', flush=True)
                # Reset selection before the next density so both begin at top.
                if scale == 1:
                    open_menu()
                    ui.key(ui.target(wid), 74)
                    ui.key(ui.target(wid), 40)
            ui.send(wid, close=True)
            ui.wait(lambda: not ui.window(title))
            assert 'APP_STATUS:0' in ui.g.shell('wait $app; echo APP_STATUS:$?')
    finally:
        ui.close()
    return dict(mode='desktop' if nested else 'native', cases=cases,
                failures=failed, private_fsck='passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='Folder containing lvedit.fxe and lvgallery.fxe')
    parser.add_argument('--guest-build', type=Path, default=ROOT/'fuse-x/build')
    parser.add_argument('--kernel', type=Path, help='Kernel executable (defaults to --guest-build/fuse-x)')
    parser.add_argument('--output', type=Path, default=ROOT/'build/fusede-wheel/guest-tests')
    parser.add_argument('--probe', action='store_true', help='Record baseline failures without stopping after the first case')
    args = parser.parse_args()
    args.kernel = args.kernel or args.guest_build/'fuse-x'
    for key in ('build', 'guest_build', 'kernel', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'result.json').unlink(missing_ok=True)
    runs = [exercise(args, nested) for nested in (False, True)]
    failures = [failure for run in runs for failure in run['failures']]
    result = dict(status='reproduced' if failures and args.probe else 'failed' if failures else 'passed',
                  runs=runs, private_images_only=True, networking='off',
                  executable_sha256={app: hashlib.sha256((args.build/(app+'.fxe')).read_bytes()).hexdigest()
                                     for app in ('lvedit', 'lvgallery')},
                  kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
                  runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(f'Graphite menu scrolling: {result["status"]}; {len(failures)} failed checks', flush=True)
    assert args.probe or not failures, '\n'.join(failures)


if __name__ == '__main__':
    main()
