#!/usr/bin/env python3
"""Check editor blur saves and explicit close decisions on private guest disks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('menu_scroll_test', PORT/'menu-scroll-test.py')
menu_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu_test)


class AutosaveGUI(menu_test.MenuGUI):
    def __init__(self, args, nested):
        self.out = args.output/('desktop' if nested else 'native')
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='graphite-editor-autosave-')
        work = Path(self.temp.name)
        self.frames, self.mailbox = work/'frames', work/'event.json'
        self.nested = nested
        apps = {name: args.build/(name+'.fxe') for name in ('lvedit', 'lvgallery')}
        apps.update({name: args.guest_build/'apps'/(name+'.fxe') for name in ('mv', 'mkdir', 'rm')})
        files = [dict(name='/dev/gfx', mode=0o20600, device=0x400),
                 dict(name='/data/home', directory=True, mode=0o40755)]
        fixtures = ['alpha-'+str(scale) for scale in (1, 2)]+['beta-'+str(scale) for scale in (1, 2)]
        fixtures += ['conflict', 'failure', 'discard']
        files += [dict(name='/data/'+name+'.txt', mode=0o100644, text=name+' original.\n')
                  for name in fixtures]
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(args.kernel), FUSE_X_DESKTOP='1' if nested else '0',
                          FUSE_X_DESKTOP_SIZE='1280x800', FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), RCC_NATIVE_AUDIO='off')
        try:
            self.g = menu_test.desktop_test.Guest(work, 'native', apps, files,
                blocks=131072, build_override=args.guest_build)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell('HOME=/data/home; export HOME; unset LVGL_SCALE')
        self.trace_at = 0

    def launch(self, *paths):
        self.trace_at = len(self.g.log)
        self.g.shell('lvedit --trace '+' '.join(map(shlex.quote, paths))+' &')
        self.g.shell('editor=$!')
        self.logged('EDIT_READY ', self.trace_at)
        wid = self.wait(lambda: self.window('Text Editor'))['id']
        if not self.nested:
            # Headless native windows have no macOS focus-gained notification.
            # Establish the real input router transition before testing loss.
            self.send(wid, input=dict(type='focus', focused=True))
        self.density(wid, 1)
        self.frame(wid, width=880, height=600)
        return wid

    def rectangles(self, at=None):
        result = {}
        for name, x, y, width, height, density in re.findall(
                rb'EDIT_RECT (\S+) (-?\d+),(-?\d+) (\d+)x(\d+) density=(\d+)',
                self.g.log[self.trace_at if at is None else at:]):
            values = list(map(int, (x, y, width, height)))
            result[name.decode()] = [value//int(density) for value in values]
        return result

    def logged(self, marker, at=0, seconds=60):
        return self.wait(lambda: marker.encode() in bytes(self.g.log[at:]).replace(b'\r', b''), seconds)

    def control(self, wid, name, at=None):
        x, y, width, height = self.wait(lambda: self.rectangles(at).get(name))
        self.local_click(wid, x+width//2, y+height//2)

    def state(self):
        text = bytes(self.g.log[self.trace_at:]).decode(errors='replace').replace('\r', '')
        headers = list(re.finditer(r'EDIT_TABS count=(\d+) active=(\d+)\n', text))
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

    def document(self, name, dirty=None, active=None):
        def expected():
            state = self.state()
            if state:
                for index, document in state['documents'].items():
                    if document['name'] == name and (dirty is None or document['dirty'] == str(dirty)) and \
                            (active is None or (index == state['active']) == active):
                        return document
            return None
        return self.wait(expected)

    def replace(self, wid, text):
        self.editor_focus(wid)
        at = len(self.g.log)
        self.send(wid, action=4)
        self.send(wid, action=2, clipboard=text)
        self.logged(f'EDIT_CHANGE bytes={len(text.encode())} dirty=1', at)
        return at

    def tab(self, wid, index):
        x, y, width, height = self.rectangles()[f'tab-{index}']
        self.local_click(wid, x+width-8, y+height//2)

    def editor_focus(self, wid):
        x, y, width, height = self.rectangles()['editor']
        self.local_click(wid, x+min(200, width//2), y+min(90, height//2))

    def path(self, wid, path, scale):
        self.frame(wid, width=880, height=600, scale=scale)
        self.control(wid, 'save-input')
        self.send(wid, action=4)
        self.send(wid, action=2, clipboard=path)
        self.key(self.target(wid), 40)

    def disk(self, path):
        output = self.g.shell('cat '+shlex.quote(path))
        return output.split('\n', 1)[1] if '\n' in output else output

    def menu_blur(self, wid, scale):
        at = len(self.g.log)
        self.control(wid, 'Help')
        self.frame(wid, width=880, height=600, scale=scale)
        return at

    def dismiss(self, wid):
        self.key(self.target(wid), 41)

    def finish(self, wid):
        self.send(wid, close=True)
        self.wait(lambda: not self.window('Text Editor'))
        assert 'EDIT_STATUS:0' in self.g.shell('wait $editor; echo EDIT_STATUS:$?')

    def autosaved(self, path, at):
        self.logged('EDIT_AUTOSAVE '+path+'\n', at)
        assert ('EDIT_AUTOSAVE_FAILED '+path).encode() not in self.g.log[at:]


def named_and_first_save(ui, scale):
    alpha, beta = f'alpha-{scale}.txt', f'beta-{scale}.txt'
    alpha_path, beta_path = '/data/'+alpha, '/data/'+beta
    wid = ui.launch(alpha_path, beta_path)
    ui.density(wid, scale)
    ui.frame(wid, width=880, height=600, scale=scale)
    ui.document(beta, dirty=0, active=True)
    clean_at = ui.menu_blur(wid, scale)
    assert not any(marker in ui.g.log[clean_at:] for marker in (b'EDIT_SAVE ', b'EDIT_AUTOSAVE ', b'EDIT_DIALOG '))
    ui.dismiss(wid)

    beta_text = f'Named menu blur {scale}x.\n'
    ui.replace(wid, beta_text)
    at = ui.menu_blur(wid, scale)
    ui.autosaved(beta_path, at)
    ui.document(beta, dirty=0, active=True)
    assert beta_text in ui.disk(beta_path)
    ui.capture(wid, f'named-menu-save-{scale}x', 880, 600, scale)
    ui.dismiss(wid)

    outgoing = f'Outgoing tab persisted at {scale}x.\n'
    ui.editor_focus(wid)
    ui.replace(wid, outgoing)
    at = len(ui.g.log)
    ui.tab(wid, 1)
    ui.autosaved(beta_path, at)
    ui.document(alpha, dirty=0, active=True)
    ui.document(beta, dirty=0, active=False)
    assert outgoing in ui.disk(beta_path)
    assert f'alpha-{scale} original.\n' in ui.disk(alpha_path)
    alpha_text = f'Keyboard tab switch persisted at {scale}x.\n'
    ui.replace(wid, alpha_text)
    at = len(ui.g.log)
    ui.ctrl(ui.target(wid), 43)
    ui.autosaved(alpha_path, at)
    ui.document(beta, dirty=0, active=True)
    ui.document(alpha, dirty=0, active=False)
    assert alpha_text in ui.disk(alpha_path) and outgoing in ui.disk(beta_path)
    window_text = f'Native window focus loss persisted at {scale}x.\n'
    ui.replace(wid, window_text)
    at = len(ui.g.log)
    ui.send(wid, input=dict(type='focus', focused=False))
    ui.autosaved(beta_path, at)
    ui.document(beta, dirty=0, active=True)
    assert window_text in ui.disk(beta_path)
    ui.send(wid, input=dict(type='focus', focused=True))
    ui.finish(wid)

    wid = ui.launch()
    ui.density(wid, scale)
    ui.frame(wid, width=880, height=600, scale=scale)
    untitled = ui.wait(ui.state)['documents'][1]['name']
    first_text = f'Untitled buffer waits for first save {scale}x.\n'
    ui.replace(wid, first_text)
    at = ui.menu_blur(wid, scale)
    ui.document(untitled, dirty=1, active=True)
    assert not any(marker in ui.g.log[at:] for marker in (b'EDIT_SAVE ', b'EDIT_AUTOSAVE ', b'EDIT_DIALOG '))
    ui.capture(wid, f'untitled-menu-no-save-{scale}x', 880, 600, scale)
    ui.dismiss(wid)
    saved_path = f'/data/first-save-{scale}.txt'
    at = len(ui.g.log)
    ui.ctrl(ui.target(wid), 22)
    ui.logged('EDIT_DIALOG save', at)
    ui.path(wid, saved_path, scale)
    ui.logged('EDIT_SAVE '+saved_path, at)
    ui.document(Path(saved_path).name, dirty=0, active=True)
    assert first_text in ui.disk(saved_path)
    subsequent = f'Named after Save As, blur saves {scale}x.\n'
    ui.replace(wid, subsequent)
    at = ui.menu_blur(wid, scale)
    ui.autosaved(saved_path, at)
    ui.document(Path(saved_path).name, dirty=0, active=True)
    assert subsequent in ui.disk(saved_path)
    ui.dismiss(wid)
    ui.finish(wid)
    print('EDITOR_AUTOSAVE named/untitled and first Save As passed at '+str(scale)+'x', flush=True)
    return dict(scale=scale, clean_blur='silent', named_menu='saved', pointer_tab='outgoing saved',
                keyboard_tab='outgoing saved', native_window_focus='saved', untitled='silent and dirty',
                first_save='explicit', subsequent_blur='saved')


def conflict_failure_and_discard(ui):
    wid = ui.launch('/data/discard.txt')
    ui.replace(wid, 'Explicit close must keep Discard meaningful.\n')
    at = len(ui.g.log)
    ui.ctrl(wid, 26)
    ui.logged('EDIT_DIALOG unsaved', at)
    ui.frame(wid, width=880, height=600)
    assert b'EDIT_AUTOSAVE ' not in ui.g.log[at:]
    ui.control(wid, 'Cancel', at)
    ui.document('discard.txt', dirty=1, active=True)
    at = len(ui.g.log)
    ui.ctrl(wid, 26)
    ui.logged('EDIT_DIALOG unsaved', at)
    ui.control(wid, 'Discard', at)
    ui.wait(lambda: ui.state() if ui.state() and ui.state()['count'] == 1 and
            ui.state()['documents'][1]['name'] != 'discard.txt' else None)
    assert 'discard original.\n' in ui.disk('/data/discard.txt')
    ui.finish(wid)

    wid = ui.launch('/data/conflict.txt', '/data/failure.txt')
    ui.tab(wid, 1)
    ui.document('conflict.txt', dirty=0, active=True)
    conflict_text = 'This draft must survive a conflict.\n'
    ui.replace(wid, conflict_text)
    ui.g.shell('echo external-content-wins > /data/conflict.txt')
    at = ui.menu_blur(wid, 1)
    ui.logged('EDIT_AUTOSAVE_FAILED /data/conflict.txt result=1\n', at)
    ui.document('conflict.txt', dirty=1, active=True)
    ui.pause(.2)
    assert ui.g.log[at:].count(b'EDIT_AUTOSAVE_FAILED /data/conflict.txt ') == 1, 'Conflict retries without another blur'
    assert b'EDIT_DIALOG ' not in ui.g.log[at:], 'Autosave opened a conflict modal'
    assert 'external-content-wins\n' in ui.disk('/data/conflict.txt')
    assert int(ui.document('conflict.txt', dirty=1)['bytes']) == len(conflict_text.encode())
    ui.capture(wid, 'autosave-conflict-keeps-draft', 880, 600, 1)
    ui.dismiss(wid)

    ui.tab(wid, 2)
    ui.document('failure.txt', dirty=0, active=True)
    failed_text = 'This draft must survive an I/O failure.\n'
    ui.replace(wid, failed_text)
    ui.g.shell('mv /data/failure.txt /data/preserved.txt')
    ui.g.shell('mkdir /data/failure.txt')
    at = ui.menu_blur(wid, 1)
    ui.logged('EDIT_AUTOSAVE_FAILED /data/failure.txt result=-1\n', at)
    ui.document('failure.txt', dirty=1, active=True)
    ui.pause(.2)
    assert ui.g.log[at:].count(b'EDIT_AUTOSAVE_FAILED /data/failure.txt ') == 1, 'I/O failure retries without another blur'
    assert b'EDIT_DIALOG ' not in ui.g.log[at:]
    assert 'failure original.\n' in ui.disk('/data/preserved.txt')
    assert int(ui.document('failure.txt', dirty=1)['bytes']) == len(failed_text.encode())
    ui.capture(wid, 'autosave-failure-keeps-draft', 880, 600, 1)
    ui.dismiss(wid)
    at = len(ui.g.log)
    ui.send(wid, close=True)
    for name in ('conflict.txt', 'failure.txt'):
        ui.logged('EDIT_DIALOG unsaved operation=2 name='+name+'\n', at)
        ui.control(wid, 'Discard', at)
    ui.wait(lambda: not ui.window('Text Editor'))
    assert 'EDIT_STATUS:0' in ui.g.shell('wait $editor; echo EDIT_STATUS:$?')
    assert 'external-content-wins\n' in ui.disk('/data/conflict.txt')
    assert 'failure original.\n' in ui.disk('/data/preserved.txt')
    return dict(named_close='Cancel/Discard preserved without autosave', conflict='dirty draft and external file retained',
                io_failure='dirty draft and original file retained', failure_policy='one nonmodal attempt per blur',
                quit='explicit Discard for both dirty documents')


def desktop_focus(ui, scale):
    path = f'/data/alpha-{scale}.txt'
    wid = ui.launch(path)
    ui.density(wid, scale)
    ui.frame(wid, width=880, height=600, scale=scale)
    content = f'Actual desktop focus switch persisted at {scale}x.\n'
    ui.replace(wid, content)
    at = len(ui.g.log)
    ui.g.shell('lvgallery --trace &')
    ui.g.shell('gallery=$!')
    other = ui.wait(lambda: ui.window('Graphite Gallery'))['id']
    ui.wait(lambda: ui.desktop().get('focused') == other)
    ui.autosaved(path, at)
    ui.document(Path(path).name, dirty=0, active=True)
    assert content in ui.disk(path)
    ui.capture(wid, f'actual-window-focus-loss-{scale}x', 880, 600, scale)
    ui.save_scene(f'actual-window-focus-loss-{scale}x')
    ui.restore(wid)
    at = len(ui.g.log)
    ui.restore(other)
    ui.pause(.2)
    assert b'EDIT_AUTOSAVE ' not in ui.g.log[at:], 'A clean focus loss wrote the file again'
    ui.send(other, close=True)
    ui.wait(lambda: not ui.window('Graphite Gallery'))
    assert 'GALLERY_STATUS:0' in ui.g.shell('wait $gallery; echo GALLERY_STATUS:$?')
    ui.finish(wid)
    print('EDITOR_AUTOSAVE desktop focus passed at '+str(scale)+'x', flush=True)
    return dict(scale=scale, actual_other_window_focus='saved', repeated_clean_focus_loss='silent')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--guest-build', type=Path, default=ROOT/'fuse-x/build')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT/'build/lvgl/editor-autosave-tests')
    args = parser.parse_args()
    args.kernel = args.kernel or args.guest_build/'fuse-x'
    for key in ('build', 'guest_build', 'kernel', 'output'):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'result.json').unlink(missing_ok=True)
    tested_runner = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report = dict(status='passed', private_images_only=True, networking='off',
                  editor_sha256=hashlib.sha256((args.build/'lvedit.fxe').read_bytes()).hexdigest(),
                  gallery_sha256=hashlib.sha256((args.build/'lvgallery.fxe').read_bytes()).hexdigest(),
                  kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
                  runner_sha256=tested_runner)
    ui = AutosaveGUI(args, False)
    try:
        report['native'] = [named_and_first_save(ui, scale) for scale in (1, 2)]
        report['failure_and_close'] = conflict_failure_and_discard(ui)
    finally:
        ui.close()
    ui = AutosaveGUI(args, True)
    try:
        report['desktop'] = [desktop_focus(ui, scale) for scale in (1, 2)]
    finally:
        ui.close()
    report['private_fsck'] = dict(native='passed', desktop='passed')
    assert hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == tested_runner, 'Runner changed while executing'
    (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Editor autosave: named/untitled, tab/window focus, Save As, conflict, I/O failure and close decisions passed', flush=True)


if __name__ == '__main__':
    main()
