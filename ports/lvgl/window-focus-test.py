#!/usr/bin/env python3
"""Check inactive Graphite inputs and resumed editing on private offline guests."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('editor_autosave_test', PORT/'editor-autosave-test.py')
autosave_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(autosave_test)


class FocusGUI(autosave_test.AutosaveGUI):
    def __init__(self, args, nested):
        self.out = args.output/('desktop' if nested else 'native')
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='graphite-window-focus-')
        work = Path(self.temp.name)
        self.frames, self.mailbox = work/'frames', work/'event.json'
        self.nested = nested
        files = [dict(name='/dev/gfx', mode=0o20600, device=0x400),
                 dict(name='/data/home', directory=True, mode=0o40755)]
        files += [dict(name=f'/data/alpha-{scale}.txt', mode=0o100644, text='Original focus fixture.\n')
                  for scale in (1, 2)]
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(args.kernel), FUSE_X_DESKTOP='1' if nested else '0',
                          FUSE_X_DESKTOP_SIZE='1280x800', FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), RCC_NATIVE_AUDIO='off')
        try:
            self.g = autosave_test.menu_test.desktop_test.Guest(work, 'native',
                {name: args.build/(name+'.fxe') for name in ('lvedit', 'lvcalc')}, files,
                blocks=131072, build_override=args.guest_build)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell('HOME=/data/home; export HOME; unset LVGL_SCALE')
        self.trace_at = 0

    def calculator(self, scale):
        at = len(self.g.log)
        self.g.shell('lvcalc --trace &')
        self.g.shell('calculator=$!')
        self.logged('CALC_READY ', at)
        window = self.wait(lambda: self.window('Calculator'))
        wid, initial_scale = window['id'], window['scale']
        values = re.findall(rb'CALC_READY [^\r\n]*entry=(-?\d+),(-?\d+) (\d+)x(\d+)', self.g.log[at:])[-1]
        rectangle = [int(value)//initial_scale for value in values]
        # CALC_READY reports coordinates relative to the padded screen's
        # content origin. Its full-width entry exposes the uniform inset.
        inset = (300-rectangle[2])//2-rectangle[0]
        rectangle[0] += inset
        rectangle[1] += inset
        if not self.nested:
            self.send(wid, input=dict(type='focus', focused=True))
        self.density(wid, scale)
        self.frame(wid, width=300, height=360, scale=scale)
        return wid, rectangle

    def focus_window(self, wid, focused):
        assert not self.nested
        self.send(wid, input=dict(type='focus', focused=focused))

    def end_calculator(self, wid):
        self.send(wid, close=True)
        self.wait(lambda: not self.window('Calculator'))
        assert 'CALC_STATUS:0' in self.g.shell('wait $calculator; echo CALC_STATUS:$?')


def accent(pixel):
    return pixel[2] > 160 and pixel[2]-pixel[0] > 40 and pixel[1]-pixel[0] > 10


def input_metrics(image, rectangle, scale):
    x, y, width, height = [value*scale for value in rectangle]
    border, accent_border, blue_inside, bright_inside = 0, 0, 0, 0
    selection = 0
    for py in range(y+3*scale, y+height-3*scale):
        for px in range(x, x+scale):
            pixel = image[2][(py*image[0]+px)*3:(py*image[0]+px)*3+3]
            border += 1
            accent_border += accent(pixel)
    for py in range(y+3*scale, y+height-3*scale):
        for px in range(x+3*scale, x+width-3*scale):
            pixel = image[2][(py*image[0]+px)*3:(py*image[0]+px)*3+3]
            blue_inside += accent(pixel)
            bright_inside += min(pixel) > 210
            # Current LVGL labels paint a white selection fill; accept the
            # shared theme's blue fill too. Ordinary text/caret is off-white.
            selection += pixel in (bytes((0x39, 0x4e, 0x70)), bytes((255, 255, 255)))
    return dict(accent_border_fraction=round(accent_border/max(1, border), 4),
                blue_inside=blue_inside, bright_inside=bright_inside, selected_pixels=selection)


def editor_visual(ui, wid, scale, state):
    image = ui.capture(wid, f'editor-{state}-{scale}x', 880, 600, scale)
    return input_metrics(image, ui.rectangles()['editor'], scale)


def native_editor(ui, scale, probe=False):
    path = f'/data/alpha-{scale}.txt'
    wid = ui.launch(path)
    ui.density(wid, scale)
    ui.frame(wid, width=880, height=600, scale=scale)
    text = 'draft-abcdef'
    ui.replace(wid, text)
    ui.key(wid, 74)
    ui.key(wid, 79)
    ui.key(wid, 79)
    focused = editor_visual(ui, wid, scale, 'focused')
    at = len(ui.g.log)
    ui.focus_window(wid, False)
    ui.autosaved(path, at)
    ui.document(Path(path).name, dirty=0, active=True)
    inactive = editor_visual(ui, wid, scale, 'inactive')
    assert focused['accent_border_fraction'] > .8 and focused['blue_inside'] > 10*scale*scale, focused
    if not probe:
        assert inactive['accent_border_fraction'] == 0 and inactive['blue_inside'] == 0, inactive
        ignored_at = len(ui.g.log)
        ui.text(wid, '9')
        ui.send(wid, action=2, clipboard='8')
        ui.pause(.1)
        assert b'EDIT_CHANGE ' not in ui.g.log[ignored_at:], 'Inactive editor accepted text or clipboard input'
    ui.focus_window(wid, True)
    resumed = editor_visual(ui, wid, scale, 'resumed')
    assert resumed['accent_border_fraction'] > .8 and resumed['blue_inside'] > 10*scale*scale, resumed
    at = len(ui.g.log)
    ui.text(wid, 'X')
    ui.logged(f'EDIT_CHANGE bytes={len(text)+1} dirty=1', at)
    at = len(ui.g.log)
    ui.focus_window(wid, False)
    ui.autosaved(path, at)
    assert 'drXaft-abcdef' in ui.disk(path), 'Window focus reset the editor caret'
    ui.focus_window(wid, True)
    if not probe:
        ui.key(wid, 74)
        ui.send(wid, input=dict(type='key', key=225, down=True))
        for _ in range(3):
            ui.key(wid, 79)
        ui.send(wid, input=dict(type='key', key=225, down=False))
        ui.focus_window(wid, False)
        ui.focus_window(wid, True)
        at = len(ui.g.log)
        ui.text(wid, 'Q')
        ui.logged('EDIT_CHANGE bytes=11 dirty=1', at)
        at = len(ui.g.log)
        ui.focus_window(wid, False)
        ui.autosaved(path, at)
        assert 'Qaft-abcdef' in ui.disk(path), 'Window focus reset the editor selection'
        ui.focus_window(wid, True)
    ui.finish(wid)
    result = dict(scale=scale, focused=focused, inactive=inactive, resumed=resumed,
                  autosave='passed', caret_position='preserved',
                  selection='not tested in baseline' if probe else 'preserved and replaced after resume')
    print('WINDOW_FOCUS native editor '+json.dumps(result, sort_keys=True), flush=True)
    return result


def calculator_visual(ui, wid, rectangle, scale, state):
    image = ui.capture(wid, f'calculator-{state}-{scale}x', 300, 360, scale)
    return input_metrics(image, rectangle, scale)


def calculator_resume(ui, wid, rectangle, scale, lose, gain):
    target = ui.target(wid)
    ui.text(target, 'c')
    focused = calculator_visual(ui, wid, rectangle, scale, 'focused-empty')
    lose()
    inactive = calculator_visual(ui, wid, rectangle, scale, 'inactive-empty')
    assert focused['accent_border_fraction'] > .8 and focused['bright_inside'] > 10*scale*scale, focused
    assert inactive['accent_border_fraction'] == 0 and inactive['bright_inside'] == 0, inactive
    if not ui.nested:
        ignored_at = len(ui.g.log)
        ui.text(wid, '9')
        ui.send(wid, action=2, clipboard='8')
        ui.pause(.1)
        assert b'CALC_INPUT ' not in ui.g.log[ignored_at:], 'Inactive calculator accepted text or clipboard input'
    gain()
    at = len(ui.g.log)
    ui.text(target, '12345')
    ui.logged('CALC_INPUT 12345\n', at)
    ui.key(target, 74)
    ui.key(target, 79)
    ui.key(target, 79)
    lose()
    gain()
    at = len(ui.g.log)
    ui.text(target, '9')
    ui.logged('CALC_INPUT 129345\n', at)
    ui.send(wid, action=4)
    selected = calculator_visual(ui, wid, rectangle, scale, 'selected')
    assert selected['selected_pixels'] > 20*scale*scale, selected
    lose()
    gain()
    resumed = calculator_visual(ui, wid, rectangle, scale, 'selection-resumed')
    assert resumed['selected_pixels'] == selected['selected_pixels'], (selected, resumed)
    at = len(ui.g.log)
    ui.text(target, '7')
    ui.logged('CALC_INPUT 7\n', at)
    ui.key(target, 40)
    ui.logged('CALC_RESULT 7\n', at)
    return dict(scale=scale, focused=focused, inactive=inactive,
                draft_and_caret='preserved', selection='preserved and replaced after resume')


def exercise(args, nested):
    ui = FocusGUI(args, nested)
    results = []
    try:
        if not nested:
            for scale in ((1,) if args.probe else (1, 2)):
                results.append(dict(editor=native_editor(ui, scale, args.probe)))
                if not args.probe:
                    wid, rectangle = ui.calculator(scale)
                    results[-1]['calculator'] = calculator_resume(ui, wid, rectangle, scale,
                        lambda: ui.focus_window(wid, False), lambda: ui.focus_window(wid, True))
                    ui.end_calculator(wid)
        else:
            for scale in (1, 2):
                path = f'/data/alpha-{scale}.txt'
                editor = ui.launch(path)
                ui.density(editor, scale)
                ui.frame(editor, width=880, height=600, scale=scale)
                ui.replace(editor, 'Desktop focus draft.')
                focused = editor_visual(ui, editor, scale, 'focused')
                at = len(ui.g.log)
                calculator, rectangle = ui.calculator(scale)
                ui.wait(lambda: ui.desktop().get('focused') == calculator)
                ui.autosaved(path, at)
                inactive = editor_visual(ui, editor, scale, 'inactive')
                assert inactive['accent_border_fraction'] == 0 and inactive['blue_inside'] == 0, inactive
                assert 'Desktop focus draft.' in ui.disk(path)
                changes = len(ui.g.log)
                calculation = calculator_resume(ui, calculator, rectangle, scale,
                    lambda: ui.restore(editor), lambda: ui.restore(calculator))
                assert b'EDIT_CHANGE ' not in ui.g.log[changes:], 'Calculator keys reached the editor'
                ui.restore(editor)
                at = len(ui.g.log)
                ui.text(0, 'Z')
                ui.logged('EDIT_CHANGE bytes=21 dirty=1', at)
                assert b'CALC_INPUT ' not in ui.g.log[at:], 'Editor keys reached the calculator'
                at = len(ui.g.log)
                ui.restore(calculator)
                ui.autosaved(path, at)
                assert 'Desktop focus draft.Z' in ui.disk(path)
                ui.click(0, 1230, 400)
                ui.wait(lambda: ui.desktop().get('focused') not in (editor, calculator))
                background_focus = ui.desktop().get('focused')
                background_calculator = calculator_visual(ui, calculator, rectangle, scale, 'desktop-inactive')
                assert background_calculator['accent_border_fraction'] == 0, background_calculator
                at = len(ui.g.log)
                ui.text(0, '9')
                ui.pause(.1)
                assert not any(marker in ui.g.log[at:] for marker in (b'CALC_INPUT ', b'EDIT_CHANGE ')), \
                    'Typing on the blank desktop reached an inactive application'
                ui.end_calculator(calculator)
                ui.finish(editor)
                result = dict(scale=scale, editor_focused=focused, editor_inactive=inactive,
                              calculator=calculation, keyboard_routing='focused app only',
                              desktop_click_focus=background_focus, autosave='passed')
                results.append(result)
                print('WINDOW_FOCUS desktop '+json.dumps(result, sort_keys=True), flush=True)
    finally:
        ui.close()
    return dict(mode='desktop' if nested else 'native', cases=results, private_fsck='passed')


def captured_drag(args):
    ui = FocusGUI(args, False)
    cases = []
    try:
        for scale in (1, 2):
            path = f'/data/alpha-{scale}.txt'
            wid = ui.launch(path)
            ui.density(wid, scale)
            ui.frame(wid, width=880, height=600, scale=scale)
            ui.replace(wid, 'abcdefghij')
            left, top, _, _ = ui.rectangles()['editor']
            first, middle, end, y = left+10, left+35, left+200, top+18
            ui.send(wid, input=dict(type='enter', inside=True))
            ui.pointer(wid, first, y)
            ui.send(wid, input=dict(type='button', button=0, down=True, x=first, y=y))
            ui.pointer(wid, middle, y)
            ui.pointer(wid, -20, y)
            ui.send(wid, input=dict(type='enter', inside=False))
            ui.send(wid, input=dict(type='enter', inside=True))
            ui.pointer(wid, end, y)
            ui.send(wid, input=dict(type='button', button=0, down=False, x=end, y=y))
            ui.capture(wid, f'captured-selection-reentered-{scale}x', 880, 600, scale)
            at = len(ui.g.log)
            ui.text(wid, 'Z')
            values = ui.wait(lambda: re.findall(rb'EDIT_CHANGE bytes=(\d+) dirty=1\r?\n', ui.g.log[at:]))
            assert int(values[-1]) == 1, 'Pointer exit ended or changed the captured selection'
            at = len(ui.g.log)
            ui.focus_window(wid, False)
            ui.autosaved(path, at)
            assert 'Z\n' not in ui.disk(path) and 'Z' in ui.disk(path)
            ui.focus_window(wid, True)
            ui.local_click(wid, end, y)
            ui.pointer(wid, first, y)
            at = len(ui.g.log)
            ui.text(wid, 'Q')
            values = ui.wait(lambda: re.findall(rb'EDIT_CHANGE bytes=(\d+) dirty=1\r?\n', ui.g.log[at:]))
            assert int(values[-1]) == 2, 'A completed drag stayed captured after button release'
            at = len(ui.g.log)
            ui.focus_window(wid, False)
            ui.autosaved(path, at)
            assert 'ZQ' in ui.disk(path)
            ui.focus_window(wid, True)
            ui.finish(wid)
            cases.append(dict(scale=scale, captured_exit_reentry='selection preserved',
                              actual_release='next click and motion no longer drag'))
    finally:
        ui.close()
    return dict(mode='native captured drag', cases=cases, private_fsck='passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--guest-build', type=Path, default=ROOT/'fuse-x/build')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT/'build/lvgl/window-focus-tests')
    parser.add_argument('--probe', action='store_true', help='Capture one baseline editor focus transition')
    parser.add_argument('--drag-only', action='store_true', help='Check a captured selection across pointer exit/reentry')
    args = parser.parse_args()
    args.kernel = args.kernel or args.guest_build/'fuse-x'
    for key in ('build', 'guest_build', 'kernel', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'result.json').unlink(missing_ok=True)
    runner_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    runs = [captured_drag(args)] if args.drag_only else \
        [exercise(args, nested) for nested in ((False,) if args.probe else (False, True))]
    assert hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == runner_hash, 'Runner changed during acceptance'
    result = dict(status='observed' if args.probe else 'passed', scope='captured drag' if args.drag_only else 'window focus', runs=runs,
                  private_images_only=True, networking='off', runner_sha256=runner_hash,
                  executable_sha256={name: hashlib.sha256((args.build/(name+'.fxe')).read_bytes()).hexdigest()
                                     for name in ('lvedit', 'lvcalc')},
                  kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest())
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('Graphite window focus: '+result['status'], flush=True)


if __name__ == '__main__':
    main()
